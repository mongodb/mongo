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

#include <algorithm>

#include "../connection_manager.h"
#include "database_model.h"
#include "workload_validation.h"

namespace test_harness {
void
workload_validation::validate(const std::string &operation_table_name,
  const std::string &schema_table_name, const std::vector<uint64_t> &known_collection_ids)
{
    WT_DECL_RET;
    wt_timestamp_t tracked_timestamp;
    std::vector<uint64_t> created_collections, deleted_collections;
    uint64_t tracked_collection_id;
    const char *tracked_key, *tracked_value;
    int tracked_op_type;
    uint64_t current_collection_id = 0;

    logger::log_msg(LOG_INFO, "Beginning validation.");

    scoped_session session = connection_manager::instance().create_session();

    /* Retrieve the collections that were created and deleted during the test. */
    parse_schema_tracking_table(
      session, schema_table_name, created_collections, deleted_collections);

    /*
     * Make sure the deleted collections do not exist on disk. The created collections are checked
     * in check_reference.
     */
    for (auto const &it : deleted_collections) {
        if (!verify_collection_file_state(session, it, false))
            testutil_die(LOG_ERROR,
              "Validation failed: collection %s present on disk while it has been tracked as "
              "deleted.",
              database::build_collection_name(it).c_str());
    }

    /*
     * All collections in memory should match those created in the schema tracking table. Dropping
     * is currently not supported.
     */
    std::sort(created_collections.begin(), created_collections.end());
    auto on_disk_collection_id = created_collections.begin();
    if (created_collections.size() != known_collection_ids.size())
        testutil_die(LOG_ERROR,
          "Validation failed: collection state mismatch, expected %lu"
          " collections to exist but have %lu on disk",
          created_collections.size(), known_collection_ids.size());
    for (const auto id : known_collection_ids) {
        if (id != *on_disk_collection_id)
            testutil_die(LOG_ERROR,
              "Validation failed: collection state mismatch expected "
              "collection id %lu but got %lu.",
              id, *on_disk_collection_id);
        on_disk_collection_id++;
    }

    /* Parse the tracking table. */
    validation_collection current_collection_records;
    scoped_cursor cursor = session.open_scoped_cursor(operation_table_name);
    while ((ret = cursor->next(cursor.get())) == 0) {
        testutil_check(
          cursor->get_key(cursor.get(), &tracked_collection_id, &tracked_key, &tracked_timestamp));
        testutil_check(cursor->get_value(cursor.get(), &tracked_op_type, &tracked_value));

        logger::log_msg(LOG_TRACE,
          "Retrieved tracked values. \n Collection id: " + std::to_string(tracked_collection_id) +
            "\n Key: " + std::string(tracked_key) +
            "\n Timestamp: " + std::to_string(tracked_timestamp) + "\n Operation type: " +
            std::to_string(tracked_op_type) + "\n Value: " + std::string(tracked_value));

        /*
         * Check if we've stepped over to the next collection. The tracking table is sorted by
         * collection_id so this is correct.
         */
        if (tracked_collection_id != current_collection_id) {
            if (std::find(known_collection_ids.begin(), known_collection_ids.end(),
                  tracked_collection_id) == known_collection_ids.end())
                testutil_die(LOG_ERROR,
                  "Validation failed: The collection id %lu is not part of the known "
                  "collection set.",
                  tracked_collection_id);
            if (tracked_collection_id < current_collection_id)
                testutil_die(LOG_ERROR, "Validation failed: The collection id %lu is out of order.",
                  tracked_collection_id);

            /*
             * Given that we've stepped over to the next collection we've built a full picture of
             * the current collection and can now validate it.
             */
            verify_collection(session, current_collection_id, current_collection_records);

            /* Begin processing the next collection. */
            current_collection_id = tracked_collection_id;
            current_collection_records.clear();
        }

        /*
         * Add the values from the tracking table to the current collection model.
         */
        update_data_model(static_cast<tracking_operation>(tracked_op_type),
          current_collection_records, current_collection_id, tracked_key, tracked_value);
    };

    /* The value of ret should be WT_NOTFOUND once the cursor has read all rows. */
    if (ret != WT_NOTFOUND)
        testutil_die(
          LOG_ERROR, "Validation failed: cursor->next() return an unexpected error %d.", ret);

    /*
     * We still need to validate the last collection. But we can also end up here if there aren't
     * any collections, check for that.
     */
    if (known_collection_ids.size() != 0)
        verify_collection(session, current_collection_id, current_collection_records);
}

void
workload_validation::parse_schema_tracking_table(scoped_session &session,
  const std::string &tracking_table_name, std::vector<uint64_t> &created_collections,
  std::vector<uint64_t> &deleted_collections)
{
    wt_timestamp_t key_timestamp;
    uint64_t key_collection_id;
    int value_operation_type;

    scoped_cursor cursor = session.open_scoped_cursor(tracking_table_name);

    while (cursor->next(cursor.get()) == 0) {
        testutil_check(cursor->get_key(cursor.get(), &key_collection_id, &key_timestamp));
        testutil_check(cursor->get_value(cursor.get(), &value_operation_type));

        logger::log_msg(LOG_TRACE, "Collection id is " + std::to_string(key_collection_id));
        logger::log_msg(LOG_TRACE, "Timestamp is " + std::to_string(key_timestamp));
        logger::log_msg(LOG_TRACE, "Operation type is " + std::to_string(value_operation_type));

        if (static_cast<tracking_operation>(value_operation_type) ==
          tracking_operation::CREATE_COLLECTION) {
            deleted_collections.erase(std::remove(deleted_collections.begin(),
                                        deleted_collections.end(), key_collection_id),
              deleted_collections.end());
            created_collections.push_back(key_collection_id);
        } else if (static_cast<tracking_operation>(value_operation_type) ==
          tracking_operation::DELETE_COLLECTION) {
            created_collections.erase(std::remove(created_collections.begin(),
                                        created_collections.end(), key_collection_id),
              created_collections.end());
            deleted_collections.push_back(key_collection_id);
        }
    }
}

void
workload_validation::update_data_model(const tracking_operation &operation,
  validation_collection &collection, const uint64_t collection_id, const char *key,
  const char *value)
{
    if (operation == tracking_operation::DELETE_KEY) {
        /* Search for the key validating that it exists. */
        const auto it = collection.find(key);
        if (it == collection.end())
            testutil_die(LOG_ERROR,
              "Validation failed: key deleted that doesn't exist. Collection id: %lu Key: %s",
              collection_id, key);
        else if (it->second.exists == false)
            /* The key has been deleted twice. */
            testutil_die(LOG_ERROR,
              "Validation failed: deleted key deleted again. Collection id: %lu Key: %s",
              collection_id, it->first.c_str());

        /* Update the key_state to deleted. */
        it->second.exists = false;
    } else if (operation == tracking_operation::INSERT)
        collection[key_value_t(key)] = key_state{true, key_value_t(value)};
    else
        testutil_die(LOG_ERROR, "Validation failed: unexpected operation in the tracking table: %d",
          static_cast<tracking_operation>(operation));
}

void
workload_validation::verify_collection(
  scoped_session &session, const uint64_t collection_id, validation_collection &collection)
{
    /* Check the collection exists on disk. */
    if (!verify_collection_file_state(session, collection_id, true))
        testutil_die(LOG_ERROR,
          "Validation failed: collection %lu not present on disk while it has been tracked as "
          "created.",
          collection_id);

    /* Walk through each key/value pair of the current collection. */
    for (const auto &record : collection)
        verify_key_value(session, collection_id, record.first, record.second);
}

bool
workload_validation::verify_collection_file_state(
  scoped_session &session, const uint64_t collection_id, bool exists) const
{
    /*
     * We don't necessarily expect to successfully open the cursor so don't create a scoped cursor.
     */
    WT_CURSOR *cursor;
    int ret = session->open_cursor(session.get(),
      database::build_collection_name(collection_id).c_str(), nullptr, nullptr, &cursor);
    if (ret == 0)
        testutil_check(cursor->close(cursor));
    return (exists ? (ret == 0) : (ret != 0));
}

void
workload_validation::verify_key_value(scoped_session &session, const uint64_t collection_id,
  const std::string &key, const key_state &key_state)
{
    WT_DECL_RET;
    const char *retrieved_value;

    scoped_cursor cursor =
      session.open_scoped_cursor(database::build_collection_name(collection_id));
    cursor->set_key(cursor.get(), key.c_str());
    ret = cursor->search(cursor.get());
    if (ret != 0) {
        if (ret == WT_NOTFOUND && key_state.exists)
            testutil_die(LOG_ERROR,
              "Validation failed: Search failed to find key that should exist. Key: %s, "
              "Collection_id: %lu",
              key.c_str(), collection_id);
        else
            testutil_die(LOG_ERROR,
              "Validation failed: Unexpected error while searching for a key. Key: %s, "
              "Collection_id: %lu",
              key.c_str(), collection_id);
    }
    if (key_state.exists == false) {
        if (ret == 0)
            testutil_die(LOG_ERROR,
              "Validation failed: Key exists when it is expected to be deleted. Key: %s, "
              "Collection_id: %lu",
              key.c_str(), collection_id);
        return;
    }
    testutil_check(cursor->get_value(cursor.get(), &retrieved_value));
    if (key_state.value != key_value_t(retrieved_value))
        testutil_die(LOG_ERROR,
          "Validation failed: Value mismatch for key. Key: %s, Collection_id: %lu, Expected "
          "value: %s, Found value: %s",
          key.c_str(), collection_id, key_state.value.c_str(), retrieved_value);
}
} // namespace test_harness
