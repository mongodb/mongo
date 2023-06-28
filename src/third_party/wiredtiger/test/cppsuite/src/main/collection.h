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

#ifndef COLLECTION_H
#define COLLECTION_H

#include <atomic>
#include <string>

namespace test_harness {

/* A collection is made of mapped key value objects. */
class collection {
public:
    collection(const uint64_t id, const uint64_t key_count, const std::string &name);

    /* Copies aren't allowed. */
    collection(const collection &) = delete;
    collection &operator=(const collection &) = delete;

    uint64_t get_key_count() const;

    /*
     * Adding new keys should generally be singly threaded per collection. If two threads both
     * attempt to add keys using the incrementing id pattern they'd frequently conflict.
     *
     * The usage pattern is:
     *   1. Call get_key_count to get the number of keys already existing. Add keys with id's equal
     *      to and greater than this value.
     *   2. Once the transaction has successfully committed then call increase_key_count() with the
     *      number of added keys.
     *
     * The set of keys should always be contiguous such that other threads calling get_key_count
     * will always know that the keys in existence are 0 -> _key_count - 1.
     */
    void increase_key_count(uint64_t increment);

public:
    const std::string name;
    const uint64_t id;

private:
    std::atomic<uint64_t> _key_count{0};
};
} // namespace test_harness

#endif
