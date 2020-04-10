/*
 * BlkAllocator.h
 *
 *  Created on: Aug 09, 2016
 *      Author: hkadayam
 */

#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include "blk.h"
#include <cassert>
#include <vector>
#include <string>
#include <thread>
#include <sstream>
#include "engine/homeds/bitmap/bitset.hpp"
#include "engine/homeds/btree/btree.hpp"
#include <folly/ThreadLocal.h>
#include <boost/range/irange.hpp>

using namespace std;

namespace homestore {

class BlkAllocConfig {
private:
    uint32_t m_blk_size;
    uint64_t m_nblks;
    std::string m_unique_name;

public:
    explicit BlkAllocConfig(const std::string& name) : BlkAllocConfig(8192, 0, name) {}
    explicit BlkAllocConfig(uint64_t nblks) : BlkAllocConfig(8192, nblks, "") {}

    BlkAllocConfig(uint32_t blk_size = 8192, uint64_t nblks = 0, const std::string& name = "") :
            m_blk_size(blk_size),
            m_nblks(nblks),
            m_unique_name(name) {}

    void set_blk_size(uint64_t blk_size) { m_blk_size = blk_size; }

    uint32_t get_blk_size() const { return m_blk_size; }

    void set_total_blks(uint64_t nblks) { m_nblks = nblks; }

    uint64_t get_total_blks() const { return m_nblks; }

    const std::string& get_name() const { return m_unique_name; }

    virtual std::string to_string() const {
        std::stringstream ss;
        ss << "Blksize=" << get_blk_size() << " TotalBlks=" << get_total_blks();
        return ss.str();
    }
};

enum BlkAllocStatus {
    BLK_ALLOC_NONE = 0,
    BLK_ALLOC_SUCCESS = 1 << 0, // Success
    BLK_ALLOC_FAILED = 1 << 1,  // Failed
    BLK_ALLOC_REQMORE = 1 << 2, // Indicate that we need more
    BLK_ALLOC_SPACEFULL = 1 << 3,
    BLK_ALLOC_INVALID_DEV = 1 << 4
};

enum BlkOpStatus {
    BLK_OP_NONE = 0,
    BLK_OP_SUCCESS = 1 << 0, // Success
    BLK_OP_FAILED = 1 << 1,  // Failed
    BLK_OP_SPACEFULL = 1 << 2,
    BLK_OP_PARTIAL_FAILED = 1 << 3
};

enum BlkAllocatorState {
    BLK_ALLOCATOR_DONE = 0,
    BLK_ALLOCATOR_WAIT_ALLOC = 1,
    BLK_ALLOCATOR_ALLOCATING = 2,
    BLK_ALLOCATOR_EXITING = 3
};

/* Hints for various allocators */
struct blk_alloc_hints {
    blk_alloc_hints() :
            desired_temp(0),
            dev_id_hint(-1),
            can_look_for_other_dev(true),
            is_contiguous(false),
            multiplier(1) {}

    uint32_t desired_temp;       // Temperature hint for the device
    int dev_id_hint;             // which physical device to pick (hint if any) -1 for don't care
    bool can_look_for_other_dev; // If alloc on device not available can I pick other device
    bool is_contiguous;
    uint32_t multiplier; // blks allocated in a blkid should be a multiple of multiplier
};

class BlkAllocator {
public:
    explicit BlkAllocator(BlkAllocConfig& cfg) { m_cfg = cfg; }

    virtual ~BlkAllocator() = default;

    virtual void inited() = 0;
    virtual BlkAllocStatus alloc(BlkId& out_blkid) = 0;
    virtual BlkAllocStatus alloc(uint8_t nblks, const blk_alloc_hints& hints, std::vector< BlkId >& out_blkid) = 0;
    virtual BlkAllocStatus alloc(uint8_t nblks, const blk_alloc_hints& hints, BlkId* out_blkid,
                                 bool best_fit = false) = 0;
    virtual bool is_blk_alloced(BlkId& in_bid) = 0;
    virtual void free(const BlkId& id) = 0;
    virtual std::string to_string() const = 0;

    virtual const BlkAllocConfig& get_config() const { return m_cfg; }

protected:
    BlkAllocConfig m_cfg;
};

/* FixedBlkAllocator is a fast allocator where it allocates only 1 size block and ALL free blocks are cached instead of
 * selectively caching few blks which are free. Thus there is no sweeping of bitmap or other to refill the cache. It
 * does not support temperature of blocks and allocates simply on first come first serve basis
 */
class FixedBlkAllocator : public BlkAllocator {
private:
    struct __fixed_blk_node {
#ifndef NDEBUG
        uint32_t this_blk_id;
#endif
        uint32_t next_blk;
    } __attribute__((__packed__));

    struct __top_blk {
        struct blob {
            uint32_t gen;
            uint32_t top_blk_id;
        } __attribute__((__packed__));

        blob b;

        __top_blk(uint64_t id) { memcpy(&b, &id, sizeof(uint64_t)); }

        __top_blk(uint32_t gen, uint32_t blk_id) {
            b.gen = gen;
            b.top_blk_id = blk_id;
        }

        uint64_t to_integer() const {
            uint64_t x;
            memcpy(&x, &b, sizeof(uint64_t));
            return x;
        }

        uint32_t get_gen() const { return b.gen; }

        uint32_t get_top_blk_id() const { return b.top_blk_id; }

        void set_gen(uint32_t gen) { b.gen = gen; }

        void set_top_blk_id(uint32_t p) { b.top_blk_id = p; }
    } __attribute__((__packed__));

    std::atomic< uint64_t > m_top_blk_id;

#ifndef NDEBUG
    std::atomic< uint32_t > m_nfree_blks;
#endif

    __fixed_blk_node* m_blk_nodes;

public:
    explicit FixedBlkAllocator(BlkAllocConfig& cfg, bool init);
    ~FixedBlkAllocator() override;

    BlkAllocStatus alloc(uint8_t nblks, const blk_alloc_hints& hints, BlkId* out_blkid, bool best_fit = false) override;
    void free(const BlkId& b) override;
    std::string to_string() const override;
    BlkAllocStatus alloc(uint8_t nblks, const blk_alloc_hints& hints, std::vector< BlkId >& out_blkid) override;
    virtual void inited() override;
    virtual BlkAllocStatus alloc(BlkId& out_blkid) override;
    virtual bool is_blk_alloced(BlkId& in_bid) override;

#ifndef NDEBUG
    uint32_t total_free_blks() const { return m_nfree_blks.load(std::memory_order_relaxed); }
#endif

private:
    void free_blk(uint32_t id);
    bool m_init;
    uint32_t m_first_blk_id;
    homeds::Bitset* m_alloc_bm;
    std::mutex m_bm_mutex;
};

} // namespace homestore
#endif
