/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

struct victim_cache_key {
    uint64_t page_id;
    uint64_t lsn;

    bool operator==(const victim_cache_key &) const = default;
};

template <> struct std::hash<victim_cache_key> {
    size_t
    operator()(const victim_cache_key &key) const noexcept
    {
        size_t h = std::hash<uint64_t>{}(key.page_id);
        h ^= std::hash<uint64_t>{}(key.lsn) << 1;
        return h;
    }
};

struct victim_cache_entry {
    uint64_t lsn;
    uint64_t backlink_lsn;
    uint64_t base_lsn;
    uint64_t backlink_checkpoint_id;
    uint64_t base_checkpoint_id;
    uint64_t delta_count;
    std::vector<uint8_t> data;
};

/*
 * One cache per page log handle, bounded by an entry count per handle and by a byte budget shared
 * across all of them, so a caller bounding the memory the page log spends gives one total rather
 * than a figure per handle. Zero means no limit; two zeros turn the cache off.
 *
 * A handle can only free its own entries. If freeing all of them would still leave too little room
 * for the page, it keeps what it has and skips the page instead. Caching is best effort, so that
 * costs a miss and nothing else.
 *
 * Room is checked before it is taken rather than in a single step, so two handles can both find
 * space and both use it. The total can exceed the budget briefly, by at most one page for each
 * handle inserting at the time.
 */
class victim_cache {
public:
    victim_cache(uint32_t max_entries, uint64_t max_bytes, std::atomic<uint64_t> &shared_bytes)
        : max_entries(max_entries), max_bytes(max_bytes), shared_bytes(shared_bytes)
    {
    }

    /* The handle is going away, so hand back whatever it still holds. */
    ~victim_cache()
    {
        shared_bytes -= local_bytes;
    }

    victim_cache(const victim_cache &) = delete;
    victim_cache &operator=(const victim_cache &) = delete;
    victim_cache(victim_cache &&) = delete;
    victim_cache &operator=(victim_cache &&) = delete;

    bool
    available() const
    {
        return max_entries > 0 || max_bytes > 0;
    }

    bool
    erase(uint64_t page_id, uint64_t lsn)
    {
        if (!available())
            return false;
        std::lock_guard<std::mutex> lock(mtx);
        auto it = map.find(victim_cache_key{page_id, lsn});
        if (it == map.end())
            return false;
        free(entry_bytes(it->second));
        map.erase(it);
        return true;
    }

    std::optional<victim_cache_entry>
    get_erase(uint64_t page_id, uint64_t lsn)
    {
        if (!available())
            return std::nullopt;
        std::lock_guard<std::mutex> lock(mtx);
        auto it = map.find(victim_cache_key{page_id, lsn});
        if (it == map.end())
            return std::nullopt;
        victim_cache_entry entry = std::move(it->second);
        free(entry_bytes(entry));
        map.erase(it);
        return entry;
    }

    void
    put(uint64_t page_id, victim_cache_entry &&entry)
    {
        if (!available())
            return;
        const uint64_t lsn = entry.lsn;
        const victim_cache_key key{page_id, lsn};
        const uint64_t cost = entry_bytes(entry);
        std::lock_guard<std::mutex> lock(mtx);

        /* Drop any copy already held under this key, whether or not the new one is cached. */
        auto it = map.find(key);
        if (it != map.end()) {
            free(entry_bytes(it->second));
            map.erase(it);
        }

        /* A page larger than the whole budget would evict everything and still not fit. */
        if (max_bytes > 0 && cost > max_bytes)
            return;

        /*
         * Work from one reading of the total. Another handle can move it underneath, but the bound
         * is approximate anyway and a stale reading costs at most a miss.
         */
        uint64_t total = shared_bytes.load();

        /* Even giving up everything this handle holds would not fit the page, so do not try. */
        if (max_bytes > 0 && total - local_bytes + cost > max_bytes)
            return;

        /* Give up this handle's own entries until the page fits under whichever bounds are set. */
        while (!map.empty() &&
          ((max_entries > 0 && map.size() >= max_entries) ||
            (max_bytes > 0 && total + cost > max_bytes))) {
            auto victim = map.begin();
            const uint64_t freed = entry_bytes(victim->second);
            free(freed);
            total -= freed;
            map.erase(victim);
        }

        allocate(cost);
        map.insert_or_assign(key, std::move(entry));
    }

    bool
    contains(uint64_t page_id, uint64_t lsn) const
    {
        if (!available())
            return false;
        std::lock_guard<std::mutex> lock(mtx);
        return map.contains(victim_cache_key{page_id, lsn});
    }

private:
    static uint64_t
    entry_bytes(const victim_cache_entry &entry)
    {
        return entry.data.capacity();
    }

    void
    allocate(uint64_t bytes)
    {
        local_bytes += bytes;
        shared_bytes += bytes;
    }

    void
    free(uint64_t bytes)
    {
        local_bytes -= bytes;
        shared_bytes -= bytes;
    }

    const uint32_t max_entries;
    const uint64_t max_bytes;
    std::atomic<uint64_t> &shared_bytes;
    uint64_t local_bytes = 0; /* This handle's share of the shared total, guarded by the lock. */
    mutable std::mutex mtx;
    std::unordered_map<victim_cache_key, victim_cache_entry> map;
};
