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

#ifndef VALIDATOR_H
#define VALIDATOR_H

#include <map>
#include <string>
#include <vector>

#include "src/main/database.h"

namespace test_harness {
struct key_state {
    key_state() = default;
    key_state(bool exists, const key_value_t &value) : exists(exists), value(value) {}
    bool exists = false;
    key_value_t value;
};
typedef std::map<key_value_t, key_state> validation_collection;
/* Class that defines a basic validation algorithm. */
class validator {
    public:
    /*
     * Validate the on disk data against what has been tracked during the test. This is done by
     * replaying the tracked operations so a representation in memory of the collections is created.
     * This representation is then compared to what is on disk.
     *
     * - operation_table_name: Table that contains all the operations performed on keys.
     * - schema_table_name: Table that contains all the schema operations performed.
     */
    void validate(
      const std::string &operation_table_name, const std::string &schema_table_name, database &db);

    private:
    /*
     * Read the tracking table to retrieve the created and deleted collections during the test.
     * collection_name: collection that contains the operations on the different collections during
     * the test.
     */
    void parse_schema_tracking_table(scoped_session &session,
      const std::string &tracking_table_name, std::vector<uint64_t> &created_collections,
      std::vector<uint64_t> &deleted_collections);

    /* Update the data model. */
    void update_data_model(const tracking_operation &operation, validation_collection &collection,
      const uint64_t collection_id, const char *key, const char *value);

    /* Compare the tracked operations against what has been saved on disk. */
    void verify_collection(
      scoped_session &session, const uint64_t collection_id, validation_collection &collection);

    /*
     * Check whether a collection exists on disk. exists: needs to be set to true if the collection
     * is expected to be existing, false otherwise.
     */
    bool verify_collection_file_state(
      scoped_session &session, const uint64_t collection_id, bool exists) const;

    /* Verify the given expected value is the same on disk. */
    void verify_key_value(scoped_session &session, const uint64_t collection_id,
      const std::string &key, const key_state &key_state);
};
} // namespace test_harness

#endif
