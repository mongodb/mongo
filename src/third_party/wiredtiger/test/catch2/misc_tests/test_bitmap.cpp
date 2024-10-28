/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <time.h>
#include <vector>

#include "wiredtiger.h"
#include "../wrappers/mock_session.h"
#include "wt_internal.h"

TEST_CASE("Chunk cache bitmap: __chunkcache_bitmap_find_free", "[bitmap]")
{
    /* Build Mock session, this will automatically create a mock connection. */
    std::shared_ptr<mock_session> session = mock_session::build_test_mock_session();

    WT_SESSION_IMPL *session_impl = session->get_wt_session_impl();
    WT_CHUNKCACHE *chunkcache;
    uint64_t capacity = 1001 + rand() % 100000;
    size_t chunk_size = 1 + rand() % 1000;
    size_t num_chunks = capacity / chunk_size;

    /* Initialize random seed: */
    srand((unsigned int)time(NULL));

    /* Setup chunk cache. This sets up the bitmap internally. */
    REQUIRE((session->get_mock_connection()->setup_chunk_cache(
              session_impl, capacity, chunk_size, chunkcache)) == 0);

    SECTION("Sequential allocation and free")
    {
        size_t bit_index;
        /* Allocate all the bits in the bitmap sequentially. */
        for (uint32_t i = 0; i < num_chunks; i++)
            REQUIRE(__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == 0);

        REQUIRE(__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == ENOSPC);

        /* Free all the bits in the bitmap sequentially. */
        for (uint32_t i = 0; i < num_chunks; i++)
            __ut_chunkcache_bitmap_free(session_impl, i);

        /* Reallocate all the bits to ensure all the frees were successful. */
        for (uint32_t i = 0; i < num_chunks; i++)
            REQUIRE(__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == 0);

        REQUIRE(__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == ENOSPC);
    }

    SECTION("Random allocation and free")
    {
        size_t bit_index, random_num_chunks;

        /* Allocate bits to the bitmap */
        for (uint32_t i = 0; i < num_chunks; i++)
            REQUIRE(__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == 0);

        REQUIRE(__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == ENOSPC);

        /* Free the bits in the bitmap randomly within range of allocated bits. */
        for (uint32_t cycle = 0; cycle < 20; cycle++) {
            /* Generate random number of chunks within range. */
            random_num_chunks = rand() % num_chunks;
            for (uint32_t i = 0; i < random_num_chunks; i++) {
                uint32_t random_bit_index = rand() % num_chunks;
                __ut_chunkcache_bitmap_free(session_impl, random_bit_index);
                REQUIRE(__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == 0);
            }
        }
    }

    SECTION("Concurrent allocations")
    {
        const size_t iterations = num_chunks;
        const uint32_t threads_num = (rand() % 100) + 1;

        std::vector<std::thread> threads;
        uint64_t allocations_made = 0;

        for (uint32_t i = 0; i < threads_num; i++) {
            /* Concurrent allocation */
            threads.emplace_back([session_impl, iterations, &allocations_made]() {
                size_t bit_index;
                for (uint32_t j = 0; j < iterations; j++) {
                    if (__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == 0)
                        __wt_atomic_add64(&allocations_made, 1);
                }
            });
        }

        /* Wait for all threads to finish */
        for (auto &thread : threads) {
            thread.join();
        }

        size_t bit_index;
        REQUIRE(allocations_made == num_chunks);
        REQUIRE(__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == ENOSPC);
    }

    SECTION("Concurrent allocations and free")
    {
        const size_t iterations = num_chunks;
        const uint32_t threads_num = (rand() % 100) + 1;

        std::vector<std::thread> threads;
        uint64_t allocations_made = 0;
        std::mutex mtx;

        for (uint32_t i = 0; i < threads_num; i++) {
            /* Concurrent allocation */
            threads.emplace_back([session_impl, iterations, &allocations_made]() {
                size_t bit_index;
                for (uint32_t j = 0; j < iterations; j++) {

                    if (__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == 0)
                        __wt_atomic_add64(&allocations_made, 1);
                }
            });

            /* Sequential release, yet interspersed with allocation threads. */
            threads.emplace_back(
              [session_impl, chunkcache, iterations, num_chunks, &allocations_made, &mtx]() {
                  for (uint32_t j = 0; j < iterations; j++) {
                      size_t bit_index = rand() % num_chunks;
                      if (!mtx.try_lock())
                          continue;
                      {
                          std::lock_guard<std::mutex> lock(mtx, std::adopt_lock);
                          if ((chunkcache->free_bitmap[bit_index / 8] &
                                (uint8_t)(0x01 << (bit_index % 8))) != 0) {
                              __ut_chunkcache_bitmap_free(session_impl, bit_index);
                              __wt_atomic_sub64(&allocations_made, 1);
                          }
                      }
                  }
              });
        }

        /* Wait for all threads to finish */
        for (auto &thread : threads) {
            thread.join();
        }

        size_t bit_index;
        for (uint32_t i = 0; i < num_chunks - allocations_made; i++) {
            REQUIRE(__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == 0);
        }
        REQUIRE(__ut_chunkcache_bitmap_alloc(session_impl, &bit_index) == ENOSPC);
    }
}
