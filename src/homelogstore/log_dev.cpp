#include "engine/homestore_base.hpp"
#include "log_dev.hpp"
#include "log_store.hpp"

namespace homestore {

SDS_LOGGING_DECL(logstore)
LogDev::LogDev() = default;
LogDev::~LogDev() = default;

void LogDev::meta_blk_found(meta_blk* mblk, sisl::byte_view buf, size_t size) {
    m_logdev_meta.meta_buf_found(buf, (void*)mblk);
}

void LogDev::start(bool format) {
    HS_ASSERT(LOGMSG, (m_append_comp_cb != nullptr), "Expected Append callback to be registered");
    HS_ASSERT(LOGMSG, (m_store_found_cb != nullptr), "Expected Log store found callback to be registered");
    HS_ASSERT(LOGMSG, (m_logfound_cb != nullptr), "Expected Logs found callback to be registered");

    m_log_records = std::make_unique< sisl::StreamTracker< log_record > >();
    m_hb = HomeStoreBase::safe_instance();
    m_stopped = false;

    // First read the info block
    if (format) {
        HS_ASSERT(LOGMSG, m_logdev_meta.is_empty(), "Expected meta to be not present");
        m_logdev_meta.create();
        m_hb->get_logdev_blkstore()->update_data_start_offset(0);
    } else {
        HS_ASSERT(LOGMSG, !m_logdev_meta.is_empty(), "Expected meta data to be read already before loading");
        auto store_list = m_logdev_meta.load();

        // Notify to the caller that a new log store was reserved earlier and it is being loaded, with its meta info
        for (auto& spair : store_list) {
            m_store_found_cb(spair.first, spair.second);
        }

        m_hb->get_logdev_blkstore()->update_data_start_offset(m_logdev_meta.get_start_dev_offset());
        do_load(m_logdev_meta.get_start_dev_offset());
        m_log_records->reinit(m_log_idx);
        m_last_flush_idx = m_log_idx - 1;
    }

    // Start a recurring timer which calls flush if pending
    m_flush_timer_hdl = iomanager.schedule_global_timer(HS_DYNAMIC_CONFIG(logstore.flush_timer_frequency_us) * 1000,
                                                        true /* recurring */, nullptr, iomgr::thread_regex::all_worker,
                                                        [this](void* cookie) { flush_if_needed(); });
}

void LogDev::stop() {
    std::condition_variable _cv;

    HS_ASSERT(LOGMSG, (m_pending_flush_size == 0),
              "LogDev stop attempted while writes to logdev are pending completion");
    bool locked_now = try_lock_flush([this, &_cv]() {
        m_stopped = true;
        _cv.notify_one();
    });

    if (!locked_now) { LOGINFOMOD(logstore, "LogDev stop is queued because of pending flush or truncation ongoing"); }

    {
        // Wait for the stopped to be completed
        std::unique_lock< std::mutex > lk(m_block_flush_q_mutex);
        _cv.wait(lk, [&] { return m_stopped; });
    }

    iomanager.cancel_timer(m_flush_timer_hdl);
    m_log_records = nullptr;
    m_logdev_meta.reset();
    m_log_idx.store(0);
    m_pending_flush_size.store(0);
    m_is_flushing.store(false);
    m_last_flush_idx = -1;
    m_last_truncate_idx = -1;
    m_last_crc = INVALID_CRC32_VALUE;
    m_block_flush_q.clear();
    m_hb = nullptr;

    LOGINFOMOD(logstore, "LogDev stopped successfully");
}

void LogDev::do_load(uint64_t device_cursor) {
    log_stream_reader lstream(device_cursor);
    logid_t loaded_from = -1;

    do {
        uint64_t group_dev_offset;
        auto buf = lstream.next_group(&group_dev_offset);
        if (buf.size() == 0) {
            assert_next_pages(lstream);
            LOGINFOMOD(logstore, "LogDev loaded log_idx in range of [{} - {}]", loaded_from, m_log_idx - 1);
            break;
        }

        log_group_header* header = (log_group_header*)buf.bytes();
        if (loaded_from == -1) { loaded_from = header->start_idx(); }

        // Loop through each record within the log group and do a callback
        auto i = 0u;
        while (i < header->nrecords()) {
            auto* rec = header->nth_record(i);
            uint32_t data_offset = (rec->offset + (rec->is_inlined ? 0 : header->oob_data_offset));

            // Do a callback on the found log entry
            sisl::byte_view b = buf;
            b.move_forward(data_offset);
            b.set_size(rec->size);
            if (m_last_truncate_idx == -1) { m_last_truncate_idx = header->start_idx() + i; }
            m_logfound_cb(rec->store_id, rec->store_seq_num, {header->start_idx() + i, group_dev_offset}, b);
            ++i;
        }
        m_log_idx = header->start_idx() + i;
    } while (true);

    // Update the tail offset with where we finally end up loading, so that new append entries can be written from here.
    auto store = m_hb->get_logdev_blkstore();
    store->update_tail_offset(store->seeked_pos());
}

void LogDev::assert_next_pages(log_stream_reader& lstream) {
    LOGINFOMOD(logstore,
               "Logdev reached offset, which has invalid header, because of end of stream. Validating if it is "
               "indeed the case or there is any corruption");

    auto cursor = lstream.group_cursor();
    for (auto i = 0u; i < HS_DYNAMIC_CONFIG(logstore->recovery_max_blks_read_for_additional_check); ++i) {
        auto buf = lstream.group_in_next_page();
        if (buf.size() != 0) {
            log_group_header* header = (log_group_header*)buf.bytes();
            HS_ASSERT_CMP(RELEASE, m_log_idx.load(std::memory_order_acquire), >, header->start_idx(),
                          "Found a header with future log_idx after reaching end of log. Hence rbuf which was read "
                          "must have been corrupted, Header: {}",
                          *header);
        }
    }
    m_hb->get_logdev_blkstore()->lseek(cursor); // Reset back
}

int64_t LogDev::append_async(logstore_id_t store_id, logstore_seq_num_t seq_num, uint8_t* data, uint32_t size,
                             void* cb_context) {
    auto idx = m_log_idx.fetch_add(1, std::memory_order_acq_rel);
    m_log_records->create(idx, store_id, seq_num, data, size, cb_context);
    flush_if_needed(size, idx);
    return idx;
}

log_buffer LogDev::read(const logdev_key& key) {
    static thread_local sisl::aligned_unique_ptr< uint8_t > _read_buf;

    // First read the offset and read the log_group. Then locate the log_idx within that and get the actual data
    // Read about 4K of buffer
    if (!_read_buf) { _read_buf = sisl::aligned_unique_ptr< uint8_t >::make_sized(dma_boundary, initial_read_size); }
    auto rbuf = _read_buf.get();
    auto store = m_hb->get_logdev_blkstore();
    store->pread((void*)rbuf, initial_read_size, key.dev_offset);

    auto header = (log_group_header*)rbuf;
    HS_ASSERT_CMP(RELEASE, header->magic_word(), ==, LOG_GROUP_HDR_MAGIC, "Log header corrupted with magic mismatch!");
    HS_ASSERT_CMP(RELEASE, header->start_idx(), <=, key.idx, "log key offset does not match with log_idx");
    HS_ASSERT_CMP(RELEASE, (header->start_idx() + header->nrecords()), >, key.idx,
                  "log key offset does not match with log_idx");
    HS_ASSERT_CMP(LOGMSG, header->total_size(), >=, header->_inline_data_offset(),
                  "Inconsistent size data in log group");

    // We can only do crc match in read if we have read all the blocks. We don't want to aggressively read more data
    // than we need to just to compare CRC for read operation. It can be done during recovery.
    if (header->total_size() <= initial_read_size) {
        crc32_t crc = crc32_ieee(init_crc32, ((uint8_t*)rbuf) + sizeof(log_group_header),
                                 header->total_size() - sizeof(log_group_header));
        HS_ASSERT_CMP(RELEASE, header->this_group_crc(), ==, crc, "CRC mismatch on read data");
    }

    serialized_log_record* rec = header->nth_record(key.idx - header->start_log_idx);
    uint32_t data_offset = (rec->offset + (rec->is_inlined ? 0 : header->oob_data_offset));

    log_buffer b((size_t)rec->size);
    if ((data_offset + b.size()) < initial_read_size) {
        std::memcpy((void*)b.bytes(), (void*)(rbuf + data_offset),
                    b.size()); // Already read them enough, copy the data
    } else {
        // Round them data offset to dma boundary in-order to make sure pread on direct io succeed. We need to skip
        // the rounded portion while copying to user buffer
        auto rounded_data_offset = sisl::round_down(data_offset, HS_STATIC_CONFIG(drive_attr.align_size));
        auto rounded_size =
            sisl::round_up(b.size() + data_offset - rounded_data_offset, HS_STATIC_CONFIG(drive_attr.align_size));

        // Allocate a fresh aligned buffer, if size cannot fit standard size
        if (rounded_size > initial_read_size) { rbuf = hs_iobuf_alloc(rounded_size); }

        LOGTRACEMOD(logstore,
                    "Addln read as data resides outside initial_read_size={} key.idx={} key.group_dev_offset={} "
                    "data_offset={} size={} rounded_data_offset={} rounded_size={}",
                    initial_read_size, key.idx, key.dev_offset, data_offset, b.size(), rounded_data_offset,
                    rounded_size);
        store->pread((void*)rbuf, rounded_size, key.dev_offset + rounded_data_offset);
        memcpy((void*)b.bytes(), (void*)(rbuf + data_offset - rounded_data_offset), b.size());

        // Free the buffer in case we allocated above
        if (rounded_size > initial_read_size) { iomanager.iobuf_free(rbuf); }
    }

    return b;
}

uint32_t LogDev::reserve_store_id() {
    std::unique_lock lg(m_meta_mutex);
    return m_logdev_meta.reserve_store(true /* persist_now */);
}

void LogDev::unreserve_store_id(uint32_t store_id) {
    std::unique_lock lg(m_meta_mutex);

    /* Get the current log_idx as marker and insert into garbage store id. Upon device truncation, these ids will
     * be reclaimed */
    auto log_id = m_log_idx.load(std::memory_order_acquire) - 1;
    m_garbage_store_ids.insert(std::pair< logid_t, logstore_id_t >(log_id, store_id));
}

void LogDev::get_registered_store_ids(std::vector< logstore_id_t >& registered, std::vector< logstore_id_t >& garbage) {
    std::unique_lock lg(m_meta_mutex);
    for (const auto& id : m_logdev_meta.reserved_store_ids()) {
        registered.push_back(id);
    }

    garbage.clear();
    for (auto& elem : m_garbage_store_ids) {
        garbage.push_back(elem.second);
    }
}

/*
 * This method prepares the log records to be flushed and returns the log_group which is fully prepared
 */
LogGroup* LogDev::prepare_flush(int32_t estimated_records) {
    int64_t flushing_upto_idx = 0u;

    assert(estimated_records > 0);
    auto lg = make_log_group((uint32_t)estimated_records);
    m_log_records->foreach_active(m_last_flush_idx + 1, [&](int64_t idx, int64_t upto_idx, log_record& record) -> bool {
        if (lg->add_record(record, idx)) {
            flushing_upto_idx = idx;
            return true;
        } else {
            return false;
        }
    });

    lg->finish();
    if (sisl_unlikely(flushing_upto_idx == 0)) { return nullptr; }
    lg->m_flush_log_idx_from = m_last_flush_idx + 1;
    lg->m_flush_log_idx_upto = flushing_upto_idx;
    HS_DEBUG_ASSERT_GE(lg->m_flush_log_idx_upto, lg->m_flush_log_idx_from,
                       "log indx upto is smaller then log indx from");
    lg->m_log_dev_offset = m_hb->get_logdev_blkstore()->alloc_next_append_blk(lg->header()->group_size);

    assert(lg->header()->oob_data_offset > 0);
    LOGDEBUGMOD(logstore, "Flushing upto log_idx={}", flushing_upto_idx);
    LOGDEBUGMOD(logstore, "Log Group: {}", *lg);
    return lg;
}

// This method checks if in case we were to add a record of size provided, do we enter into a state which exceeds
// our threshold. If so, it first flushes whats accumulated so far and then add the pending flush size counter with
// the new record size
void LogDev::flush_if_needed(const uint32_t new_record_size, logid_t new_idx) {
    // If after adding the record size, if we have enough to flush or if its been too much time before we actually
    // flushed, attempt to flush by setting the atomic bool variable.
    auto pending_sz = m_pending_flush_size.fetch_add(new_record_size, std::memory_order_relaxed) + new_record_size;
    bool flush_by_size = (pending_sz >= LogDev::flush_data_threshold_size());
    bool flush_by_time = !flush_by_size && pending_sz &&
        (get_elapsed_time_us(m_last_flush_time) > HS_DYNAMIC_CONFIG(logstore.max_time_between_flush_us));

    if ((iomanager.am_i_worker_reactor() || iomanager.am_i_tight_loop_reactor()) && (flush_by_size || flush_by_time)) {
        bool expected_flushing = false;
        if (m_is_flushing.compare_exchange_strong(expected_flushing, true, std::memory_order_acq_rel)) {
            LOGTRACEMOD(
                logstore,
                "Flushing now because either pending_size={} is greater than data_threshold={} or elapsed time since "
                "last flush={} us is greater than max_time_between_flush={} us",
                pending_sz, LogDev::flush_data_threshold_size(), get_elapsed_time_us(m_last_flush_time),
                HS_DYNAMIC_CONFIG(logstore.max_time_between_flush_us));

            // We were able to win the flushing competition and now we gather all the flush data and reserve a slot.
            if (new_idx == -1) new_idx = m_log_idx.load(std::memory_order_relaxed) - 1;
            if (m_last_flush_idx >= new_idx) {
                LOGTRACEMOD(logstore, "Log idx {} is just flushed", new_idx);
                unlock_flush();
                return;
            }
            auto lg = prepare_flush(new_idx - m_last_flush_idx + 4); // Estimate 4 more extra in case of parallel writes
            if (sisl_unlikely(!lg)) {
                LOGTRACEMOD(logstore, "Log idx {} last_flush_idx {} prepare flush failed", new_idx, m_last_flush_idx);
                unlock_flush();
                return;
            }
            m_pending_flush_size.fetch_sub(lg->actual_data_size(), std::memory_order_relaxed);

            COUNTER_INCREMENT_IF_ELSE(home_log_store_mgr.m_metrics, flush_by_size, logdev_flush_by_size_count,
                                      logdev_flush_by_timer_count, 1);
            m_last_flush_time = Clock::now();
            LOGTRACEMOD(logstore, "Flush prepared, flushing data size={}", lg->actual_data_size());
            do_flush(lg);
        } else {
            LOGTRACEMOD(logstore, "Back to back flushing, will let the current flush to finish and perform this flush");
            COUNTER_INCREMENT(home_log_store_mgr.m_metrics, logdev_back_to_back_flushing, 1);
        }
    }
}

void LogDev::do_flush(LogGroup* lg) {
    auto* store = m_hb->get_logdev_blkstore();
    // auto offset = store->reserve(lg->data_size() + sizeof(log_group_header));

    HISTOGRAM_OBSERVE(home_log_store_mgr.m_metrics, logdev_flush_records_distribution, lg->nrecords());
    HISTOGRAM_OBSERVE(home_log_store_mgr.m_metrics, logdev_flush_size_distribution, lg->actual_data_size());
    auto req = logdev_req::make_request();
    req->m_log_group = lg;
    store->pwritev(lg->iovecs().data(), (int)lg->iovecs().size(), lg->m_log_dev_offset, req);
}

void LogDev::process_logdev_completions(const boost::intrusive_ptr< blkstore_req< BlkBuffer > >& bs_req) {
    auto req = to_logdev_req(bs_req);
    if (req->is_read) {
        // update logdev read metrics;
    } else {
        // update logdev write metrics;
        on_flush_completion(req->m_log_group);
    }
}

void LogDev::on_flush_completion(LogGroup* lg) {
    LOGTRACEMOD(logstore, "Flush completed for logid[{} - {}]", lg->m_flush_log_idx_from, lg->m_flush_log_idx_upto);
    m_log_records->complete(lg->m_flush_log_idx_from, lg->m_flush_log_idx_upto);
    m_last_flush_idx = lg->m_flush_log_idx_upto;
    auto flush_ld_key = logdev_key{m_last_flush_idx, lg->m_log_dev_offset};

    for (auto idx = lg->m_flush_log_idx_from; idx <= lg->m_flush_log_idx_upto; ++idx) {
        auto& record = m_log_records->at(idx);
        m_append_comp_cb(record.store_id, logdev_key{idx, lg->m_log_dev_offset}, flush_ld_key,
                         lg->m_flush_log_idx_upto - idx, record.context);
    }

    m_last_crc = lg->header()->cur_grp_crc;
    unlock_flush();
}

bool LogDev::try_lock_flush(const flush_blocked_callback& cb) {
    std::unique_lock lk(m_block_flush_q_mutex);
    if (m_stopped) {
        LOGWARNMOD(logstore, "Trying to lock a flush on a stopped logdev, not locking the flush");
        return false;
    }

    bool expected_flushing = false;
    if (m_is_flushing.compare_exchange_strong(expected_flushing, true, std::memory_order_acq_rel)) {
        cb();
        return true;
    }

    // Flushing is blocked already, add it to the callback q
    m_block_flush_q.emplace_back(cb);
    return false;
}

void LogDev::unlock_flush() {
    if (m_block_flush_q.size() > 0) {
        std::unique_lock lk(m_block_flush_q_mutex);
        for (auto& cb : m_block_flush_q) {
            if (m_stopped) {
                LOGINFOMOD(logstore, "Logdev is stopped and thus not processing outstanding flush_lock_q");
                return;
            }
            cb();
        }
        m_block_flush_q.clear();
    }
    m_is_flushing.store(false, std::memory_order_release);

    // Try to do chain flush if its really needed.
    LOGTRACEMOD(logstore, "Unlocked the flush, try doing chain flushing if needed");
    flush_if_needed();
}

void LogDev::truncate(const logdev_key& key) {
    auto store = m_hb->get_logdev_blkstore();

    LOGINFOMOD(logstore, "Truncating log device upto log_id={} vdev_offset={} truncated {} log records", key.idx,
               key.dev_offset, key.idx - m_last_truncate_idx);
    m_log_records->truncate(key.idx);
    store->truncate(key.dev_offset);
    m_last_truncate_idx = key.idx;

    {
        std::unique_lock< std::mutex > lk(m_meta_mutex);

        // Update the start offset to be read upon restart
        m_logdev_meta.update_start_dev_offset(key.dev_offset, false /* persist_now */);

        // Now that store is truncated, we can reclaim the store ids which are garbaged (if any) earlier
        for (auto it = m_garbage_store_ids.cbegin(); it != m_garbage_store_ids.cend();) {
            if (it->first > key.idx) break;

            LOGINFOMOD(logstore, "Garbage collecting the log store id {} log_idx={}", it->second, it->first);
            m_logdev_meta.unreserve_store(it->second, false /* persist_now */);
            it = m_garbage_store_ids.erase(it);
        }

        m_logdev_meta.persist();
    }
}

void LogDev::update_store_meta(const logstore_id_t idx, const logstore_meta& meta, bool persist_now) {
    std::unique_lock lg(m_meta_mutex);
    m_logdev_meta.update_store_meta(idx, meta, persist_now);
}

/////////////////////////////// LogDevMetadata Section ///////////////////////////////////////
logdev_superblk* LogDevMetadata::create() {
    auto req_sz = required_sb_size(0);
    m_raw_buf = hs_create_byte_view(req_sz, meta_blk_mgr->is_aligned_buf_needed(req_sz));
    m_sb = new (m_raw_buf.bytes()) logdev_superblk();

    logstore_meta* m = (logstore_meta*)m_sb->store_meta;
    std::fill_n(m, store_capacity(), logstore_meta::default_value());

    m_id_reserver = std::make_unique< sisl::IDReserver >(store_capacity());
    persist();
    return m_sb;
}

void LogDevMetadata::reset() {
    m_raw_buf = sisl::byte_view{};
    m_sb = nullptr;
    m_meta_mgr_cookie = nullptr;
    m_id_reserver.reset();
    m_store_info.clear();
}

void LogDevMetadata::meta_buf_found(const sisl::byte_view& buf, void* meta_cookie) {
    m_meta_mgr_cookie = meta_cookie;
    m_raw_buf = buf;
    m_sb = (logdev_superblk*)m_raw_buf.bytes();
}

std::vector< std::pair< logstore_id_t, logstore_meta > > LogDevMetadata::load() {
    std::vector< std::pair< logstore_id_t, logstore_meta > > ret_list;
    ret_list.reserve(1024);
    m_id_reserver = std::make_unique< sisl::IDReserver >(store_capacity());

    HS_RELEASE_ASSERT_NE(m_raw_buf.bytes(), nullptr, "Load called without getting metadata");
    HS_RELEASE_ASSERT_LE(m_sb->get_version(), logdev_superblk::LOGDEV_SB_VERSION, "Logdev super blk version mismatch");

    logstore_meta* smeta = (logstore_meta*)m_sb->store_meta;
    logstore_id_t idx = 0;
    auto n = 0u;
    while (n < m_sb->num_stores) {
        if (logstore_meta::is_valid(smeta[idx])) {
            m_store_info.insert(idx);
            ret_list.push_back(std::make_pair<>(idx, smeta[idx]));
            ++n;
        }
        ++idx;
    }

    return ret_list;
}

void LogDevMetadata::persist() {
    if (m_meta_mgr_cookie) {
        meta_blk_mgr->update_sub_sb((void*)m_raw_buf.bytes(), m_raw_buf.size(), m_meta_mgr_cookie);
    } else {
        meta_blk_mgr->add_sub_sb("LOG_DEV", (void*)m_raw_buf.bytes(), m_raw_buf.size(), m_meta_mgr_cookie);
    }
}

logstore_id_t LogDevMetadata::reserve_store(bool persist_now) {
    auto idx = m_id_reserver->reserve(); // Search the id reserver and alloc an idx;
    m_store_info.insert(idx);

    // Write the meta inforation on-disk meta
    resize_if_needed(); // In case the idx falls out of the alloc boundary, resize them
    logstore_meta* smeta = (logstore_meta*)m_sb->store_meta;
    logstore_meta::init(smeta[idx]);
    ++m_sb->num_stores;
    if (persist_now) { persist(); }

    return idx;
}

void LogDevMetadata::unreserve_store(logstore_id_t idx, bool persist_now) {
    m_id_reserver->unreserve(idx); // Search the id reserver and alloc an idx;
    m_store_info.erase(idx);

    bool shrunk = resize_if_needed(); // In case the idx unregistered falls out of boundary, we can shrink them
    if (!shrunk) {
        // We have not shrunk the store info, so we need to explicitly clear the store meta in on-disk meta
        logstore_meta* smeta = (logstore_meta*)m_sb->store_meta;
        logstore_meta::clear(smeta[idx]);
    }
    --m_sb->num_stores;
    if (persist_now) { persist(); }
}

void LogDevMetadata::update_store_meta(const logstore_id_t idx, const logstore_meta& meta, bool persist_now) {
    // Update the in-memory copy
    m_store_info.insert(idx);

    // Update the on-disk copy
    resize_if_needed();
    logstore_meta* smeta = (logstore_meta*)m_sb->store_meta;
    smeta[idx] = meta;

    if (persist_now) { persist(); }
}

logstore_meta& LogDevMetadata::mutable_store_meta(const logstore_id_t idx) {
    logstore_meta* smeta = (logstore_meta*)m_sb->store_meta;
    return smeta[idx];
}

void LogDevMetadata::update_start_dev_offset(uint64_t offset, bool persist_now) {
    m_sb->start_dev_offset = offset;
    if (persist_now) { persist(); }
}

bool LogDevMetadata::resize_if_needed() {
    auto req_sz = required_sb_size((m_store_info.size() == 0) ? 0 : *m_store_info.rbegin());
    if (req_sz != m_raw_buf.size()) {
        auto old_buf = m_raw_buf;

        auto m_raw_buf = hs_create_byte_view(req_sz, meta_blk_mgr->is_aligned_buf_needed(req_sz));
        m_sb = new (m_raw_buf.bytes()) logdev_superblk();

        logstore_meta* m = (logstore_meta*)m_sb->store_meta;
        std::fill_n(m, store_capacity(), logstore_meta::default_value());

        std::memcpy(m_raw_buf.bytes(), old_buf.bytes(), std::min(old_buf.size(), m_raw_buf.size()));
        return true;
    } else {
        return false;
    }
}

uint32_t LogDevMetadata::store_capacity() const {
    return (m_raw_buf.size() - sizeof(logdev_superblk)) / sizeof(logstore_meta);
}
} // namespace homestore
