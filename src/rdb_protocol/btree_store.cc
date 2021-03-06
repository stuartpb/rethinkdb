// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/store.hpp"  // NOLINT(build/include_order)

#include <functional>  // NOLINT(build/include_order)

#include "arch/runtime/coroutines.hpp"
#include "btree/depth_first_traversal.hpp"
#include "btree/node.hpp"
#include "btree/operations.hpp"
#include "btree/secondary_operations.hpp"
#include "btree/slice.hpp"
#include "buffer_cache/alt.hpp"
#include "buffer_cache/cache_balancer.hpp"
#include "concurrency/wait_any.hpp"
#include "containers/archive/buffer_stream.hpp"
#include "containers/archive/vector_stream.hpp"
#include "containers/archive/versioned.hpp"
#include "containers/disk_backed_queue.hpp"
#include "containers/scoped.hpp"
#include "logger.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/erase_range.hpp"
#include "rdb_protocol/protocol.hpp"
#include "serializer/config.hpp"
#include "stl_utils.hpp"

// Some of this implementation is in store.cc and some in btree_store.cc for no
// particularly good reason.  Historically it turned out that way, and for now
// there's not enough refactoring urgency to combine them into one.
//
// (Really, some of the implementation is also in rdb_protocol/btree.cc.)

sindex_not_ready_exc_t::sindex_not_ready_exc_t(
        std::string sindex_name,
        const secondary_index_t &sindex,
        const std::string &table_name) {
    if (sindex.being_deleted) {
        rassert(false, "A query tried to access index `%s` on table `%s` which is "
                       "currently being deleted. Queries should not be able to "
                       "access such indexes in the first place, so this is a bug.",
                sindex_name.c_str(),
                table_name.c_str());
        info = strprintf("Index `%s` on table `%s` "
                         "was accessed while it was being deleted.",
                         sindex_name.c_str(),
                         table_name.c_str());
    } else {
        rassert(!sindex.post_construction_complete);
        info = strprintf("Index `%s` on table `%s` "
                         "was accessed before its construction was finished.",
                         sindex_name.c_str(),
                         table_name.c_str());
    }
}

const char* sindex_not_ready_exc_t::what() const throw() {
    return info.c_str();
}

sindex_not_ready_exc_t::~sindex_not_ready_exc_t() throw() { }

store_t::store_t(serializer_t *serializer,
                 cache_balancer_t *balancer,
                 const std::string &perfmon_name,
                 bool create,
                 perfmon_collection_t *parent_perfmon_collection,
                 rdb_context_t *_ctx,
                 io_backender_t *io_backender,
                 const base_path_t &base_path,
                 outdated_index_report_t *_index_report,
                 namespace_id_t _table_id)
    : store_view_t(region_t::universe()),
      perfmon_collection(),
      io_backender_(io_backender), base_path_(base_path),
      perfmon_collection_membership(parent_perfmon_collection, &perfmon_collection, perfmon_name),
      ctx(_ctx),
      changefeed_server((ctx == NULL || ctx->manager == NULL)
                        ? NULL
                        : new ql::changefeed::server_t(ctx->manager)),
      index_report(_index_report),
      table_id(_table_id)
{
    cache.init(new cache_t(serializer, balancer, &perfmon_collection));
    general_cache_conn.init(new cache_conn_t(cache.get()));

    if (create) {
        vector_stream_t key;
        // The version used when deserializing this data depends on the block magic.
        // The block magic set by init_superblock corresponds to the latest version
        // and so this serialization does too.
        // VSI: Do this better.
        write_message_t wm;
        region_t kr = region_t::universe();
        serialize_for_metainfo(&wm, kr);
        key.reserve(wm.size());
        int res = send_write_message(&key, &wm);
        guarantee(!res);

        txn_t txn(general_cache_conn.get(), write_durability_t::HARD,
                  repli_timestamp_t::distant_past, 1);
        buf_lock_t superblock(&txn, SUPERBLOCK_ID, alt_create_t::create);
        btree_slice_t::init_superblock(&superblock, key.vector(), binary_blob_t());
        real_superblock_t sb(std::move(superblock));
        create_stat_block(&sb);
    }

    btree.init(new btree_slice_t(cache.get(),
                                 &perfmon_collection,
                                 "primary",
                                 index_type_t::PRIMARY));

    // Initialize sindex slices
    {
        // Since this is the btree constructor, nothing else should be locking these
        // things yet, so this should work fairly quickly and does not need a real
        // interruptor.
        cond_t dummy_interruptor;
        write_token_t token;
        new_write_token(&token);
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        acquire_superblock_for_write(repli_timestamp_t::distant_past,
                                     1,
                                     write_durability_t::SOFT,
                                     &token,
                                     &txn,
                                     &superblock,
                                     &dummy_interruptor);

        buf_lock_t sindex_block(superblock->expose_buf(),
                                superblock->get_sindex_block_id(),
                                access_t::write);

        std::map<sindex_name_t, secondary_index_t> sindexes;
        get_secondary_indexes(&sindex_block, &sindexes);

        for (auto it = sindexes.begin(); it != sindexes.end(); ++it) {
            // Deleted secondary indexes should not be added to the perfmons
            perfmon_collection_t *pc =
                it->first.being_deleted
                ? NULL
                : &perfmon_collection;
            auto slice = make_scoped<btree_slice_t>(cache.get(),
                                                    pc,
                                                    it->first.name,
                                                    index_type_t::SECONDARY);
            secondary_index_slices.insert(std::make_pair(it->second.id,
                                                         std::move(slice)));
        }

        update_outdated_sindex_list(&sindex_block);
    }

    help_construct_bring_sindexes_up_to_date();
}

store_t::~store_t() {
    assert_thread();
    drainer.drain();

    if (index_report != NULL) {
        index_report->destroy();
    }
}

void store_t::read(
        DEBUG_ONLY(const metainfo_checker_t& metainfo_checker, )
        const read_t &read,
        read_response_t *response,
        UNUSED order_token_t order_token,  // TODO
        read_token_t *token,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;

    acquire_superblock_for_read(token, &txn, &superblock,
                                interruptor,
                                read.use_snapshot());

    DEBUG_ONLY(check_metainfo(DEBUG_ONLY(metainfo_checker, ) superblock.get());)

    protocol_read(read, response, superblock.get(), interruptor);
}

void store_t::write(
        DEBUG_ONLY(const metainfo_checker_t& metainfo_checker, )
        const region_map_t<binary_blob_t>& new_metainfo,
        const write_t &write,
        write_response_t *response,
        const write_durability_t durability,
        state_timestamp_t timestamp,
        UNUSED order_token_t order_token,  // TODO
        write_token_t *token,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> real_superblock;
    const int expected_change_count = 2; // FIXME: this is incorrect, but will do for now
    acquire_superblock_for_write(timestamp.to_repli_timestamp(),
                                 expected_change_count, durability, token,
                                 &txn, &real_superblock, interruptor);

    check_and_update_metainfo(DEBUG_ONLY(metainfo_checker, ) new_metainfo,
                              real_superblock.get());
    scoped_ptr_t<superblock_t> superblock(real_superblock.release());
    protocol_write(write, response, timestamp, &superblock, interruptor);
}

// TODO: Figure out wtf does the backfill filtering, figure out wtf constricts delete range operations to hit only a certain hash-interval, figure out what filters keys.
bool store_t::send_backfill(
        const region_map_t<state_timestamp_t> &start_point,
        send_backfill_callback_t *send_backfill_cb,
        traversal_progress_combiner_t *progress,
        read_token_t *token,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    acquire_superblock_for_backfill(token, &txn, &superblock, interruptor);

    buf_lock_t sindex_block(superblock->expose_buf(), superblock->get_sindex_block_id(),
                            access_t::read);

    region_map_t<binary_blob_t> unmasked_metainfo;
    get_metainfo_internal(superblock->get(), &unmasked_metainfo);
    region_map_t<binary_blob_t> metainfo = unmasked_metainfo.mask(start_point.get_domain());
    if (send_backfill_cb->should_backfill(metainfo)) {
        protocol_send_backfill(start_point, send_backfill_cb, superblock.get(), &sindex_block, progress, interruptor);
        return true;
    }
    return false;
}

void store_t::throttle_backfill_chunk(signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    // Warning: No re-ordering is allowed during throttling!

    // As long as our secondary index post construction is implemented using a
    // secondary index mod queue, it doesn't make sense from a performance point
    // of view to run secondary index post construction and backfilling at the same
    // time. We pause backfilling while any index post construction is going on on
    // this store by acquiring a read lock on `backfill_postcon_lock`.
    rwlock_in_line_t lock_acq(&backfill_postcon_lock, access_t::read);
    wait_any_t waiter(lock_acq.read_signal(), interruptor);
    waiter.wait_lazily_ordered();
    if (interruptor->is_pulsed()) {
        throw interrupted_exc_t();
    }
}

struct backfill_chunk_timestamp_t : public boost::static_visitor<repli_timestamp_t> {
    repli_timestamp_t operator()(const backfill_chunk_t::delete_key_t &del) const {
        return del.recency;
    }

    repli_timestamp_t operator()(const backfill_chunk_t::delete_range_t &) const {
        return repli_timestamp_t::distant_past;
    }

    repli_timestamp_t operator()(const backfill_chunk_t::key_value_pairs_t &kv) const {
        repli_timestamp_t most_recent = repli_timestamp_t::distant_past;
        rassert(!kv.backfill_atoms.empty());
        for (size_t i = 0; i < kv.backfill_atoms.size(); ++i) {
            most_recent = superceding_recency(most_recent, kv.backfill_atoms[i].recency);
        }
        return most_recent;
    }

    repli_timestamp_t operator()(const backfill_chunk_t::sindexes_t &) const {
        return repli_timestamp_t::distant_past;
    }
};

void store_t::receive_backfill(
        const backfill_chunk_t &chunk,
        write_token_t *token,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();
    with_priority_t p(CORO_PRIORITY_BACKFILL_RECEIVER);

    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> real_superblock;
    const int expected_change_count = 1; // TODO: this is not correct

    // We use HARD durability because we want backfilling to be throttled if we
    // receive data faster than it can be written to disk. Otherwise we might
    // exhaust the cache's dirty page limit and bring down the whole table.
    // Other than that, the hard durability guarantee is not actually
    // needed here.
    acquire_superblock_for_write(boost::apply_visitor(backfill_chunk_timestamp_t(),
                                                      chunk.val),
                                 expected_change_count,
                                 write_durability_t::HARD,
                                 token,
                                 &txn,
                                 &real_superblock,
                                 interruptor);

    scoped_ptr_t<superblock_t> superblock(real_superblock.release());
    protocol_receive_backfill(std::move(superblock),
                              interruptor,
                              chunk);
}

void store_t::maybe_drop_all_sindexes(const binary_blob_t &zero_metainfo,
                                      const write_durability_t durability,
                                      signal_t *interruptor) {
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;

    const int expected_change_count = 1;
    write_token_t token;
    new_write_token(&token);

    acquire_superblock_for_write(repli_timestamp_t::distant_past,
                                 expected_change_count,
                                 durability,
                                 &token,
                                 &txn,
                                 &superblock,
                                 interruptor);

    region_map_t<binary_blob_t> regions;
    get_metainfo_internal(superblock->get(), &regions);

    bool empty_region = true;
    for (auto it = regions.begin(); it != regions.end(); ++it) {
        if (it->second.size() != 0 && it->second != zero_metainfo) {
            empty_region = false;
        }
    }

    // If we are hosting no regions, we can blow away secondary indexes
    if (empty_region) {
        buf_lock_t sindex_block(superblock->expose_buf(),
                                superblock->get_sindex_block_id(),
                                access_t::write);

        std::map<sindex_name_t, secondary_index_t> sindexes;
        ::get_secondary_indexes(&sindex_block, &sindexes);

        for (auto const &sindex : sindexes) {
            if (!sindex.first.being_deleted) {
                bool success = drop_sindex(sindex.first, &sindex_block);
                guarantee(success);
            }
        }
    }
}

void store_t::reset_data(
        const binary_blob_t &zero_metainfo,
        const region_t &subregion,
        const write_durability_t durability,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();
    with_priority_t p(CORO_PRIORITY_RESET_DATA);

    // Check if secondary indexes should be dropped - if the store is not hosting data
    // for any ranges.
    maybe_drop_all_sindexes(zero_metainfo, durability, interruptor);

    // Erase the data in small chunks
    always_true_key_tester_t key_tester;
    const uint64_t max_erased_per_pass = 100;
    for (done_traversing_t done_erasing = done_traversing_t::NO;
         done_erasing == done_traversing_t::NO;) {
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;

        const int expected_change_count = 2 + max_erased_per_pass;
        write_token_t token;
        new_write_token(&token);
        acquire_superblock_for_write(repli_timestamp_t::distant_past,
                                     expected_change_count,
                                     durability,
                                     &token,
                                     &txn,
                                     &superblock,
                                     interruptor);        

        buf_lock_t sindex_block(superblock->expose_buf(),
                                superblock->get_sindex_block_id(),
                                access_t::write);

        /* Note we don't allow interruption during this step; it's too easy to end up in
        an inconsistent state. */
        cond_t non_interruptor;

        rdb_live_deletion_context_t deletion_context;
        std::vector<rdb_modification_report_t> mod_reports;
        key_range_t deleted_range;
        done_erasing = rdb_erase_small_range(btree.get(),
                                             &key_tester,
                                             subregion.inner,
                                             superblock.get(),
                                             &deletion_context,
                                             &non_interruptor,
                                             max_erased_per_pass,
                                             &mod_reports,
                                             &deleted_range);

        region_map_t<binary_blob_t> old_metainfo;
        get_metainfo_internal(superblock->get(), &old_metainfo);
        region_map_t<binary_blob_t> new_metainfo = old_metainfo;
        region_t deleted_region(subregion.beg, subregion.end, deleted_range);
        new_metainfo.set(deleted_region, zero_metainfo);
        update_metainfo(old_metainfo, new_metainfo, superblock.get());

        superblock.reset();
        if (!mod_reports.empty()) {
            update_sindexes(txn.get(), &sindex_block, mod_reports, true);
        }
    }
}

scoped_ptr_t<new_mutex_in_line_t> store_t::get_in_line_for_sindex_queue(
        buf_lock_t *sindex_block) {
    assert_thread();
    // The line for the sindex queue is there to guarantee that we push things to
    // the sindex queue(s) in the same order in which we held the sindex block.
    // It allows us to release the sindex block before starting to push data to the
    // sindex queues without sacrificing that guarantee.
    // Pushing data to those queues can take a while, and we wouldn't want to block
    // other transactions while we are doing that.

    guarantee(!sindex_block->empty());
    if (sindex_block->access() == access_t::write) {
        sindex_block->write_acq_signal()->wait();
    } else {
        // `bring_sindexes_up_to_date()` calls `lock_sindex_queue()` with only a read
        // acquisition. It is ok in that context, and we have to handle it here.
        sindex_block->read_acq_signal()->wait();
    }
    return scoped_ptr_t<new_mutex_in_line_t>(
            new new_mutex_in_line_t(&sindex_queue_mutex));
}

void store_t::register_sindex_queue(
            internal_disk_backed_queue_t *disk_backed_queue,
            const new_mutex_in_line_t *acq) {
    assert_thread();
    acq->acq_signal()->wait_lazily_unordered();

    for (std::vector<internal_disk_backed_queue_t *>::iterator it = sindex_queues.begin();
            it != sindex_queues.end(); ++it) {
        guarantee(*it != disk_backed_queue);
    }
    sindex_queues.push_back(disk_backed_queue);
}

void store_t::deregister_sindex_queue(
            internal_disk_backed_queue_t *disk_backed_queue,
            const new_mutex_in_line_t *acq) {
    assert_thread();
    acq->acq_signal()->wait_lazily_unordered();

    for (std::vector<internal_disk_backed_queue_t *>::iterator it = sindex_queues.begin();
            it != sindex_queues.end(); ++it) {
        if (*it == disk_backed_queue) {
            sindex_queues.erase(it);
            return;
        }
    }
}

void store_t::emergency_deregister_sindex_queue(
    internal_disk_backed_queue_t *disk_backed_queue) {
    assert_thread();
    drainer.assert_draining();
    new_mutex_in_line_t acq(&sindex_queue_mutex);
    acq.acq_signal()->wait_lazily_unordered();

    deregister_sindex_queue(disk_backed_queue, &acq);
}

void store_t::update_sindexes(
            txn_t *txn,
            buf_lock_t *sindex_block,
            const std::vector<rdb_modification_report_t> &mod_reports,
            bool release_sindex_block) {
    scoped_ptr_t<new_mutex_in_line_t> acq =
            get_in_line_for_sindex_queue(sindex_block);
    {
        sindex_access_vector_t sindexes;
        acquire_post_constructed_sindex_superblocks_for_write(
                sindex_block, &sindexes);
        if (release_sindex_block) {
            sindex_block->reset_buf_lock();
        }

        rdb_live_deletion_context_t deletion_context;
        for (size_t i = 0; i < mod_reports.size(); ++i) {
            rdb_update_sindexes(this,
                                sindexes,
                                &mod_reports[i],
                                txn,
                                &deletion_context,
                                NULL,
                                NULL,
                                NULL);
        }
    }

    // Write mod reports onto the sindex queue. We are in line for the
    // sindex_queue mutex and can already release all other locks.
    sindex_queue_push(mod_reports, acq.get());
}

void store_t::sindex_queue_push(const rdb_modification_report_t &mod_report,
                                const new_mutex_in_line_t *acq) {
    assert_thread();
    acq->acq_signal()->wait_lazily_unordered();

    if (!sindex_queues.empty()) {
        // This is for a disk backed queue so there's no versioning issues.
        // (deserializating_viewer_t in disk_backed_queue.hpp also uses the
        // LATEST_DISK version, and such queues are ephemeral).
        write_message_t wm;
        serialize<cluster_version_t::LATEST_DISK>(&wm, mod_report);

        for (auto it = sindex_queues.begin(); it != sindex_queues.end(); ++it) {
            (*it)->push(wm);
        }
    }
}

void store_t::sindex_queue_push(
        const std::vector<rdb_modification_report_t> &mod_reports,
        const new_mutex_in_line_t *acq) {
    assert_thread();
    acq->acq_signal()->wait_lazily_unordered();

    if (!sindex_queues.empty()) {
        scoped_array_t<write_message_t> wms(mod_reports.size());
        for (size_t i = 0; i < mod_reports.size(); ++i) {
            // This is for a disk backed queue so there are no versioning issues.
            serialize<cluster_version_t::LATEST_DISK>(&wms[i], mod_reports[i]);
        }

        for (auto it = sindex_queues.begin(); it != sindex_queues.end(); ++it) {
            (*it)->push(wms);
        }
    }
}

void store_t::add_progress_tracker(
        map_insertion_sentry_t<uuid_u, const parallel_traversal_progress_t *> *sentry,
        uuid_u id, const parallel_traversal_progress_t *p) {
    assert_thread();
    sentry->reset(&progress_trackers, id, p);
}

progress_completion_fraction_t store_t::get_progress(uuid_u id) {
    if (!std_contains(progress_trackers, id)) {
        return progress_completion_fraction_t();
    } else {
        return progress_trackers[id]->guess_completion();
    }
}

bool store_t::add_sindex(
        const sindex_name_t &name,
        const std::vector<char> &opaque_definition,
        buf_lock_t *sindex_block) {
    secondary_index_t sindex;
    if (::get_secondary_index(sindex_block, name, &sindex)) {
        return false; // sindex was already created
    } else {
        {
            buf_lock_t sindex_superblock(sindex_block, alt_create_t::create);
            sindex.superblock = sindex_superblock.block_id();
            sindex.opaque_definition = opaque_definition;

            /* Notice we're passing in empty strings for metainfo. The metainfo in
             * the sindexes isn't used for anything but this could perhaps be
             * something that would give a better error if someone did try to use
             * it... on the other hand this code isn't exactly idiot proof even
             * with that. */
            btree_slice_t::init_superblock(&sindex_superblock,
                                           std::vector<char>(),
                                           binary_blob_t());
        }

        secondary_index_slices.insert(
                std::make_pair(sindex.id,
                               make_scoped<btree_slice_t>(cache.get(),
                                                          &perfmon_collection,
                                                          name.name,
                                                          index_type_t::SECONDARY)));

        sindex.post_construction_complete = false;

        ::set_secondary_index(sindex_block, name, sindex);
        return true;
    }
}

sindex_name_t compute_sindex_deletion_name(uuid_u sindex_uuid) {
    sindex_name_t result("_DEL_" + uuid_to_str(sindex_uuid));
    result.being_deleted = true;
    return result;
}

class clear_sindex_traversal_cb_t
        : public depth_first_traversal_callback_t {
public:
    clear_sindex_traversal_cb_t() {
        collected_keys.reserve(CHUNK_SIZE);
    }
    done_traversing_t handle_pair(scoped_key_value_t &&keyvalue) {
        collected_keys.push_back(store_key_t(keyvalue.key()));
        if (collected_keys.size() >= CHUNK_SIZE) {
            return done_traversing_t::YES;
        } else {
            return done_traversing_t::NO;
        }
    }
    const std::vector<store_key_t> &get_keys() const {
        return collected_keys;
    }
    static const size_t CHUNK_SIZE = 32;
private:
    std::vector<store_key_t> collected_keys;
};

void store_t::clear_sindex(
        secondary_index_t sindex,
        value_sizer_t *sizer,
        const deletion_context_t *deletion_context,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {

    /* Delete one piece of the secondary index at a time */
    for (bool reached_end = false; !reached_end;)
    {
        /* Start a transaction (1). */
        write_token_t token;
        new_write_token(&token);
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        acquire_superblock_for_write(
            repli_timestamp_t::distant_past,
            // Not really the right value, since many keys will share a leaf node:
            clear_sindex_traversal_cb_t::CHUNK_SIZE,
            write_durability_t::SOFT,
            &token,
            &txn,
            &superblock,
            interruptor);

        /* Get the sindex block (1). */
        buf_lock_t sindex_block(superblock->expose_buf(),
                                superblock->get_sindex_block_id(),
                                access_t::write);
        superblock->release();

        /* Clear part of the index data */
        buf_lock_t sindex_superblock_lock(buf_parent_t(&sindex_block),
                                          sindex.superblock, access_t::write);
        sindex_block.reset_buf_lock();
        scoped_ptr_t<superblock_t> sindex_superblock
            = make_scoped<real_superblock_t>(std::move(sindex_superblock_lock));

        /* 1. Collect a bunch of keys to delete */
        clear_sindex_traversal_cb_t traversal_cb;
        reached_end = btree_depth_first_traversal(sindex_superblock.get(),
                                 key_range_t::universe(),
                                 &traversal_cb,
                                 direction_t::FORWARD,
                                 release_superblock_t::KEEP);

        /* 2. Actually delete them */
        const std::vector<store_key_t> &keys = traversal_cb.get_keys();
        for (size_t i = 0; i < keys.size(); ++i) {
            promise_t<superblock_t *> superblock_promise;
            {
                keyvalue_location_t kv_location;
                find_keyvalue_location_for_write(sizer, sindex_superblock.release(),
                        keys[i].btree_key(), deletion_context->balancing_detacher(),
                        &kv_location, NULL /* stats */, NULL /* trace */,
                        &superblock_promise);

                deletion_context->in_tree_deleter()->delete_value(
                    buf_parent_t(&kv_location.buf), kv_location.value.get());
                kv_location.value.reset();
                if (kv_location.there_originally_was_value) {
                    buf_write_t write(&kv_location.buf);
                    auto leaf_node = static_cast<leaf_node_t *>(write.get_data_write());
                    leaf::remove(sizer,
                                 leaf_node,
                                 keys[i].btree_key(),
                                 repli_timestamp_t::distant_past,
                                 key_modification_proof_t::real_proof());
                }
                check_and_handle_underfull(sizer, &kv_location.buf,
                        &kv_location.last_buf, kv_location.superblock,
                        keys[i].btree_key(),
                        deletion_context->balancing_detacher());

                /* Here kv_location is destructed, which returns the superblock */
            }

            /* Reclaim the sindex superblock for the next deletion */
            sindex_superblock.init(superblock_promise.wait());
        }
    }

    {
        /* Start a transaction (2). */
        write_token_t token;
        new_write_token(&token);
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        acquire_superblock_for_write(
            repli_timestamp_t::distant_past,
            2,
            write_durability_t::SOFT,
            &token,
            &txn,
            &superblock,
            interruptor);

        /* Get the sindex block (2). */
        buf_lock_t sindex_block(superblock->expose_buf(),
                                superblock->get_sindex_block_id(),
                                access_t::write);
        superblock->release();

        /* The root node should have been emptied at this point. Delete it */
        {
            buf_lock_t sindex_superblock_lock(buf_parent_t(&sindex_block),
                                              sindex.superblock, access_t::write);
            real_superblock_t sindex_superblock(std::move(sindex_superblock_lock));
            if (sindex_superblock.get_root_block_id() != NULL_BLOCK_ID) {
                buf_lock_t root_node(sindex_superblock.expose_buf(),
                                     sindex_superblock.get_root_block_id(),
                                     access_t::write);
                {
                    /* Guarantee that the root is actually empty. */
                    buf_read_t rread(&root_node);
                    const node_t *node = static_cast<const node_t *>(
                            rread.get_data_read());
                    if (node::is_internal(node)) {
                        const internal_node_t *root_int_node
                            = static_cast<const internal_node_t *>(
                                rread.get_data_read());
                        /* This just prints a warning in release mode for now,
                        because leaking a few blocks is probably better than
                        making the database inaccessible. */
                        rassert(root_int_node->npairs == 0);
                        if (root_int_node->npairs != 0) {
                            logWRN("The secondary index tree was not empty after "
                                   "clearing it. We are leaking some data blocks. "
                                   "Please report this issue at "
                                   "https://github.com/rethinkdb/rethinkdb/issues/");
                        }
                    }
                    /* If the root is a leaf we are good */
                }
                /* Delete the root */
                root_node.write_acq_signal()->wait_lazily_unordered();
                root_node.mark_deleted();
            }
            if (sindex_superblock.get_stat_block_id() != NULL_BLOCK_ID) {
                buf_lock_t stat_block(sindex_superblock.expose_buf(),
                                      sindex_superblock.get_stat_block_id(),
                                      access_t::write);
                stat_block.write_acq_signal()->wait_lazily_unordered();
                stat_block.mark_deleted();
            }
            if (sindex_superblock.get_sindex_block_id() != NULL_BLOCK_ID) {
                buf_lock_t sind_block(sindex_superblock.expose_buf(),
                                      sindex_superblock.get_sindex_block_id(),
                                      access_t::write);
                sind_block.write_acq_signal()->wait_lazily_unordered();
                sind_block.mark_deleted();
            }
        }
        /* Now it's safe to completely delete the index */
        buf_lock_t sindex_superblock_lock(buf_parent_t(&sindex_block),
                                          sindex.superblock, access_t::write);
        sindex_superblock_lock.write_acq_signal()->wait_lazily_unordered();
        sindex_superblock_lock.mark_deleted();
        ::delete_secondary_index(&sindex_block, compute_sindex_deletion_name(sindex.id));
        size_t num_erased = secondary_index_slices.erase(sindex.id);
        guarantee(num_erased == 1);
    }
}

bool secondary_indexes_are_equivalent(const std::vector<char> &left,
                                      const std::vector<char> &right) {
    sindex_disk_info_t sindex_info_left;
    sindex_disk_info_t sindex_info_right;
    deserialize_sindex_info(left, &sindex_info_left);
    deserialize_sindex_info(right, &sindex_info_right);

    if (sindex_info_left.multi == sindex_info_right.multi &&
        sindex_info_left.geo == sindex_info_right.geo &&
        sindex_info_left.mapping_version_info.original_reql_version ==
            sindex_info_right.mapping_version_info.original_reql_version) {
        // Need to determine if the mapping function is the same, re-serialize them
        // and compare the vectors
        bool res;
        write_message_t wm_left;
        vector_stream_t stream_left;
        serialize_for_version(cluster_version_t::CLUSTER, &wm_left,
                              sindex_info_left.mapping);
        res = send_write_message(&stream_left, &wm_left);
        guarantee(res == 0);

        write_message_t wm_right;
        vector_stream_t stream_right;
        serialize_for_version(cluster_version_t::CLUSTER, &wm_right,
                              sindex_info_right.mapping);
        res = send_write_message(&stream_right, &wm_right);
        guarantee(res == 0);

        return stream_left.vector() == stream_right.vector();
    }

    return false;
}

void store_t::set_sindexes(
        const std::map<sindex_name_t, secondary_index_t> &sindexes,
        buf_lock_t *sindex_block,
        std::set<sindex_name_t> *created_sindexes_out)
    THROWS_ONLY(interrupted_exc_t) {

    std::map<sindex_name_t, secondary_index_t> existing_sindexes;
    ::get_secondary_indexes(sindex_block, &existing_sindexes);

    for (auto it = existing_sindexes.begin(); it != existing_sindexes.end(); ++it) {
        if (!std_contains(sindexes, it->first) && !it->first.being_deleted) {
            bool success = drop_sindex(it->first, sindex_block);
            guarantee(success);
        }
    }

    for (auto it = sindexes.begin(); it != sindexes.end(); ++it) {
        auto extant_index = existing_sindexes.find(it->first);
        if (extant_index != existing_sindexes.end() &&
            !secondary_indexes_are_equivalent(it->second.opaque_definition,
                                              extant_index->second.opaque_definition)) {
            bool success = drop_sindex(extant_index->first, sindex_block);
            guarantee(success);
            extant_index = existing_sindexes.end();
        }

        if (extant_index == existing_sindexes.end()) {
            bool success = add_sindex(it->first,
                                      it->second.opaque_definition,
                                      sindex_block);
            guarantee(success);
            created_sindexes_out->insert(it->first);
        }
    }
}

bool store_t::mark_index_up_to_date(const sindex_name_t &name,
                                    buf_lock_t *sindex_block)
    THROWS_NOTHING {
    secondary_index_t sindex;
    bool found = ::get_secondary_index(sindex_block, name, &sindex);

    if (found) {
        sindex.post_construction_complete = true;

        ::set_secondary_index(sindex_block, name, sindex);
    }

    return found;
}

bool store_t::mark_index_up_to_date(uuid_u id,
                                    buf_lock_t *sindex_block)
    THROWS_NOTHING {
    secondary_index_t sindex;
    bool found = ::get_secondary_index(sindex_block, id, &sindex);

    if (found) {
        sindex.post_construction_complete = true;

        ::set_secondary_index(sindex_block, id, sindex);
    }

    return found;
}

void store_t::rename_sindex(
        const sindex_name_t &old_name,
        const sindex_name_t &new_name,
        buf_lock_t *sindex_block)
        THROWS_ONLY(interrupted_exc_t) {
    secondary_index_t old_sindex;
    bool success = get_secondary_index(sindex_block, old_name, &old_sindex);
    guarantee(success);

    // Drop the new sindex (if it exists)
    UNUSED bool b = drop_sindex(new_name, sindex_block);

    // Delete the current entry
    success = delete_secondary_index(sindex_block, old_name);
    guarantee(success);
    set_secondary_index(sindex_block, new_name, old_sindex);

    // Rename the perfmons
    guarantee(!old_name.being_deleted);
    guarantee(!new_name.being_deleted);
    auto slice_it = secondary_index_slices.find(old_sindex.id);
    guarantee(slice_it != secondary_index_slices.end());
    guarantee(slice_it->second.has());
    slice_it->second->assert_thread();
    slice_it->second->stats.rename(&perfmon_collection, new_name.name,
                                   index_type_t::SECONDARY);

    if (index_report != NULL) {
        index_report->index_renamed(old_name.name, new_name.name);
    }
}

MUST_USE bool store_t::drop_sindex(
        const sindex_name_t &name,
        buf_lock_t *sindex_block)
        THROWS_ONLY(interrupted_exc_t) {
    guarantee(!name.being_deleted);
    /* Remove reference in the super block */
    secondary_index_t sindex;
    if (!::get_secondary_index(sindex_block, name, &sindex)) {
        return false;
    } else {
        /* Mark the secondary index as deleted */
        bool success = mark_secondary_index_deleted(sindex_block, name);
        guarantee(success);

        /* Clear the sindex later. It starts its own transaction and we don't
        want to deadlock because we're still holding locks. */
        coro_t::spawn_sometime(std::bind(&store_t::delayed_clear_sindex,
                                         this,
                                         sindex,
                                         drainer.lock()));
    }

    if (index_report != NULL) {
        index_report->index_dropped(name.name);
    }

    return true;
}

MUST_USE bool store_t::mark_secondary_index_deleted(
        buf_lock_t *sindex_block,
        const sindex_name_t &name) {
    secondary_index_t sindex;
    bool success = get_secondary_index(sindex_block, name, &sindex);
    if (!success) {
        return false;
    }

    // Delete the current entry
    success = delete_secondary_index(sindex_block, name);
    guarantee(success);

    // Insert the new entry under a different name
    const sindex_name_t sindex_del_name = compute_sindex_deletion_name(sindex.id);
    sindex.being_deleted = true;
    set_secondary_index(sindex_block, sindex_del_name, sindex);

    // Hide the index from the perfmon collection
    auto slice_it = secondary_index_slices.find(sindex.id);
    guarantee(slice_it != secondary_index_slices.end());
    guarantee(slice_it->second.has());
    slice_it->second->assert_thread();
    slice_it->second->stats.hide();

    return true;
}

MUST_USE bool store_t::acquire_sindex_superblock_for_read(
        const sindex_name_t &name,
        const std::string &table_name,
        superblock_t *superblock,
        scoped_ptr_t<real_superblock_t> *sindex_sb_out,
        std::vector<char> *opaque_definition_out,
        uuid_u *sindex_uuid_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    assert_thread();
    rassert(opaque_definition_out != NULL);
    rassert(sindex_uuid_out != NULL);

    /* Acquire the sindex block. */
    buf_lock_t sindex_block(superblock->expose_buf(), superblock->get_sindex_block_id(),
                            access_t::read);
    superblock->release();

    /* Figure out what the superblock for this index is. */
    secondary_index_t sindex;
    if (!::get_secondary_index(&sindex_block, name, &sindex)) {
        return false;
    }

    *opaque_definition_out = sindex.opaque_definition;
    *sindex_uuid_out = sindex.id;

    if (!sindex.is_ready()) {
        throw sindex_not_ready_exc_t(name.name, sindex, table_name);
    }

    buf_lock_t superblock_lock(&sindex_block, sindex.superblock, access_t::read);
    sindex_block.reset_buf_lock();
    sindex_sb_out->init(new real_superblock_t(std::move(superblock_lock)));
    return true;
}

MUST_USE bool store_t::acquire_sindex_superblock_for_write(
        const sindex_name_t &name,
        const std::string &table_name,
        superblock_t *superblock,
        scoped_ptr_t<real_superblock_t> *sindex_sb_out,
        uuid_u *sindex_uuid_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    assert_thread();
    rassert(sindex_uuid_out != NULL);

    /* Get the sindex block. */
    buf_lock_t sindex_block(superblock->expose_buf(),
                            superblock->get_sindex_block_id(),
                            access_t::write);
    superblock->release();

    /* Figure out what the superblock for this index is. */
    secondary_index_t sindex;
    if (!::get_secondary_index(&sindex_block, name, &sindex)) {
        return false;
    }
    *sindex_uuid_out = sindex.id;

    if (!sindex.is_ready()) {
        throw sindex_not_ready_exc_t(name.name, sindex, table_name);
    }


    buf_lock_t superblock_lock(&sindex_block, sindex.superblock,
                               access_t::write);
    sindex_block.reset_buf_lock();
    sindex_sb_out->init(new real_superblock_t(std::move(superblock_lock)));
    return true;
}

store_t::sindex_access_t::sindex_access_t(btree_slice_t *_btree,
                                          sindex_name_t _name,
                                          secondary_index_t _sindex,
                                          scoped_ptr_t<real_superblock_t> _superblock)
    : btree(_btree),
      name(std::move(_name)),
      sindex(std::move(_sindex)),
      superblock(std::move(_superblock))
{ }

store_t::sindex_access_t::~sindex_access_t() { }


void store_t::acquire_all_sindex_superblocks_for_write(
        block_id_t sindex_block_id,
        buf_parent_t parent,
        sindex_access_vector_t *sindex_sbs_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    assert_thread();

    /* Get the sindex block. */
    buf_lock_t sindex_block(parent, sindex_block_id, access_t::write);

    acquire_all_sindex_superblocks_for_write(&sindex_block, sindex_sbs_out);
}

void store_t::acquire_all_sindex_superblocks_for_write(
        buf_lock_t *sindex_block,
        sindex_access_vector_t *sindex_sbs_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    acquire_sindex_superblocks_for_write(
            boost::optional<std::set<sindex_name_t> >(),
            sindex_block,
            sindex_sbs_out);
}

void store_t::acquire_post_constructed_sindex_superblocks_for_write(
        buf_lock_t *sindex_block,
        sindex_access_vector_t *sindex_sbs_out)
    THROWS_NOTHING {
    assert_thread();
    std::set<sindex_name_t> sindexes_to_acquire;
    std::map<sindex_name_t, secondary_index_t> sindexes;
    ::get_secondary_indexes(sindex_block, &sindexes);

    for (auto it = sindexes.begin(); it != sindexes.end(); ++it) {
        /* Note that this can include indexes currently being deleted.
        In fact it must include those indexes if they had been post constructed
        before, since there might still be ongoing transactions traversing the
        index despite it being under deletion.*/
        if (it->second.post_construction_complete) {
            sindexes_to_acquire.insert(it->first);
        }
    }

    acquire_sindex_superblocks_for_write(
            sindexes_to_acquire, sindex_block,
            sindex_sbs_out);
}

bool store_t::acquire_sindex_superblocks_for_write(
            boost::optional<std::set<sindex_name_t> > sindexes_to_acquire, //none means acquire all sindexes
            buf_lock_t *sindex_block,
            sindex_access_vector_t *sindex_sbs_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    assert_thread();

    std::map<sindex_name_t, secondary_index_t> sindexes;
    ::get_secondary_indexes(sindex_block, &sindexes);

    for (auto it = sindexes.begin(); it != sindexes.end(); ++it) {
        if (sindexes_to_acquire && !std_contains(*sindexes_to_acquire, it->first)) {
            continue;
        }

        /* Getting the slice and asserting we're on the right thread. */
        btree_slice_t *sindex_slice = secondary_index_slices.at(it->second.id).get();
        sindex_slice->assert_thread();

        buf_lock_t superblock_lock(
                sindex_block, it->second.superblock, access_t::write);

        sindex_sbs_out->push_back(
                make_scoped<sindex_access_t>(
                        get_sindex_slice(it->second.id),
                        it->first,
                        it->second,
                        make_scoped<real_superblock_t>(std::move(superblock_lock))));
    }

    //return's true if we got all of the sindexes requested.
    return sindex_sbs_out->size() == sindexes_to_acquire->size();
}

bool store_t::acquire_sindex_superblocks_for_write(
            boost::optional<std::set<uuid_u> > sindexes_to_acquire, //none means acquire all sindexes
            buf_lock_t *sindex_block,
            sindex_access_vector_t *sindex_sbs_out)
    THROWS_ONLY(sindex_not_ready_exc_t) {
    assert_thread();

    std::map<sindex_name_t, secondary_index_t> sindexes;
    ::get_secondary_indexes(sindex_block, &sindexes);

    for (auto it = sindexes.begin(); it != sindexes.end(); ++it) {
        if (sindexes_to_acquire && !std_contains(*sindexes_to_acquire, it->second.id)) {
            continue;
        }

        /* Getting the slice and asserting we're on the right thread. */
        btree_slice_t *sindex_slice = secondary_index_slices.at(it->second.id).get();
        sindex_slice->assert_thread();

        buf_lock_t superblock_lock(
                sindex_block, it->second.superblock, access_t::write);

        sindex_sbs_out->push_back(
                make_scoped<sindex_access_t>(
                        get_sindex_slice(it->second.id),
                        it->first,
                        it->second,
                        make_scoped<real_superblock_t>(std::move(superblock_lock))));
    }

    //return's true if we got all of the sindexes requested.
    return sindex_sbs_out->size() == sindexes_to_acquire->size();
}

void store_t::check_and_update_metainfo(
        DEBUG_ONLY(const metainfo_checker_t& metainfo_checker, )
        const region_map_t<binary_blob_t> &new_metainfo,
        real_superblock_t *superblock) const
        THROWS_NOTHING {
    assert_thread();
    region_map_t<binary_blob_t> old_metainfo
        = check_metainfo(DEBUG_ONLY(metainfo_checker, ) superblock);
    update_metainfo(old_metainfo, new_metainfo, superblock);
}

region_map_t<binary_blob_t>
store_t::check_metainfo(
        DEBUG_ONLY(const metainfo_checker_t& metainfo_checker, )
        real_superblock_t *superblock) const
        THROWS_NOTHING {
    assert_thread();
    region_map_t<binary_blob_t> old_metainfo;
    get_metainfo_internal(superblock->get(), &old_metainfo);
#ifndef NDEBUG
    metainfo_checker.check_metainfo(old_metainfo.mask(metainfo_checker.get_domain()));
#endif
    return old_metainfo;
}

void store_t::update_metainfo(const region_map_t<binary_blob_t> &old_metainfo,
                              const region_map_t<binary_blob_t> &new_metainfo,
                              real_superblock_t *superblock)
    const THROWS_NOTHING {
    assert_thread();
    region_map_t<binary_blob_t> updated_metadata = old_metainfo;
    updated_metadata.update(new_metainfo);

    rassert(updated_metadata.get_domain() == region_t::universe());

    buf_lock_t *sb_buf = superblock->get();
    // Clear the existing metainfo. This makes sure that we completely rewrite
    // the metainfo. That avoids two issues:
    // - `set_superblock_metainfo()` wouldn't remove any deleted keys
    // - `set_superblock_metainfo()` is more efficient if we don't do any
    //   in-place updates in its current implementation.
    clear_superblock_metainfo(sb_buf);

    std::vector<std::vector<char> > keys;
    std::vector<binary_blob_t> values;
    keys.reserve(updated_metadata.size());
    values.reserve(updated_metadata.size());
    for (region_map_t<binary_blob_t>::const_iterator i = updated_metadata.begin();
         i != updated_metadata.end();
         ++i) {
        vector_stream_t key;
        write_message_t wm;
        serialize_for_metainfo(&wm, i->first);
        key.reserve(wm.size());
        DEBUG_VAR int res = send_write_message(&key, &wm);
        rassert(!res);

        keys.push_back(std::move(key.vector()));
        values.push_back(i->second);
    }

    set_superblock_metainfo(sb_buf, keys, values);
}

void store_t::do_get_metainfo(UNUSED order_token_t order_token,  // TODO
                              read_token_t *token,
                              signal_t *interruptor,
                              region_map_t<binary_blob_t> *out)
    THROWS_ONLY(interrupted_exc_t) {
    assert_thread();
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    acquire_superblock_for_read(token,
                                &txn, &superblock,
                                interruptor,
                                false /* KSI: christ */);

    get_metainfo_internal(superblock->get(), out);
}

void store_t::
get_metainfo_internal(buf_lock_t *sb_buf,
                      region_map_t<binary_blob_t> *out)
    const THROWS_NOTHING {
    assert_thread();
    std::vector<std::pair<std::vector<char>, std::vector<char> > > kv_pairs;
    // TODO: this is inefficient, cut out the middleman (vector)
    get_superblock_metainfo(sb_buf, &kv_pairs);

    std::vector<std::pair<region_t, binary_blob_t> > result;
    for (std::vector<std::pair<std::vector<char>, std::vector<char> > >::iterator i = kv_pairs.begin(); i != kv_pairs.end(); ++i) {
        const std::vector<char> &value = i->second;

        region_t region;
        {
            buffer_read_stream_t key(i->first.data(), i->first.size());
            archive_result_t res = deserialize_for_metainfo(&key, &region);
            guarantee_deserialization(res, "region");
        }

        result.push_back(std::make_pair(region, binary_blob_t(value.begin(), value.end())));
    }
    region_map_t<binary_blob_t> res(result.begin(), result.end());
    rassert(res.get_domain() == region_t::universe());
    *out = res;
}

void store_t::set_metainfo(const region_map_t<binary_blob_t> &new_metainfo,
                           UNUSED order_token_t order_token,  // TODO
                           write_token_t *token,
                           signal_t *interruptor) THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    acquire_superblock_for_write(repli_timestamp_t::distant_past,
                                 1,
                                 write_durability_t::HARD,
                                 token,
                                 &txn,
                                 &superblock,
                                 interruptor);

    region_map_t<binary_blob_t> old_metainfo;
    get_metainfo_internal(superblock->get(), &old_metainfo);
    update_metainfo(old_metainfo, new_metainfo, superblock.get());
}

void store_t::acquire_superblock_for_read(
        read_token_t *token,
        scoped_ptr_t<txn_t> *txn_out,
        scoped_ptr_t<real_superblock_t> *sb_out,
        signal_t *interruptor,
        bool use_snapshot)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    object_buffer_t<fifo_enforcer_sink_t::exit_read_t>::destruction_sentinel_t destroyer(&token->main_read_token);
    wait_interruptible(token->main_read_token.get(), interruptor);

    cache_snapshotted_t cache_snapshotted =
        use_snapshot ? CACHE_SNAPSHOTTED_YES : CACHE_SNAPSHOTTED_NO;
    get_btree_superblock_and_txn_for_reading(
        general_cache_conn.get(), cache_snapshotted, sb_out, txn_out);
}

void store_t::acquire_superblock_for_backfill(
        read_token_t *token,
        scoped_ptr_t<txn_t> *txn_out,
        scoped_ptr_t<real_superblock_t> *sb_out,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    object_buffer_t<fifo_enforcer_sink_t::exit_read_t>::destruction_sentinel_t destroyer(&token->main_read_token);
    wait_interruptible(token->main_read_token.get(), interruptor);

    get_btree_superblock_and_txn_for_backfilling(general_cache_conn.get(),
                                                 btree->get_backfill_account(),
                                                 sb_out, txn_out);
}

void store_t::acquire_superblock_for_write(
        repli_timestamp_t timestamp,
        int expected_change_count,
        write_durability_t durability,
        write_token_t *token,
        scoped_ptr_t<txn_t> *txn_out,
        scoped_ptr_t<real_superblock_t> *sb_out,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    assert_thread();

    object_buffer_t<fifo_enforcer_sink_t::exit_write_t>::destruction_sentinel_t destroyer(&token->main_write_token);
    wait_interruptible(token->main_write_token.get(), interruptor);

    get_btree_superblock_and_txn(general_cache_conn.get(), write_access_t::write,
                                 expected_change_count, timestamp,
                                 durability, sb_out, txn_out);
}

/* store_view_t interface */
void store_t::new_read_token(read_token_t *token_out) {
    assert_thread();
    fifo_enforcer_read_token_t token = main_token_source.enter_read();
    token_out->main_read_token.create(&main_token_sink, token);
}

void store_t::new_write_token(write_token_t *token_out) {
    assert_thread();
    fifo_enforcer_write_token_t token = main_token_source.enter_write();
    token_out->main_write_token.create(&main_token_sink, token);
}
