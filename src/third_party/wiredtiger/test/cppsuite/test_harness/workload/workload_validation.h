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

#ifndef WORKLOAD_VALIDATION_H
#define WORKLOAD_VALIDATION_H

#include <string>

extern "C" {
#include "wiredtiger.h"
}

#include "database_model.h"

namespace test_harness {

/*
 * Class that can validate database state and collection data.
 */
class workload_validation {
    public:
    /*
     * Validate the on disk data against what has been tracked during the test. This is done by
     * replaying the tracked operations so a representation in memory of the collections is created.
     * This representation is then compared to what is on disk. operation_table_name: collection
     * that contains all the operations about the key/value pairs in the different collections used
     * during the test. schema_table_name: collection that contains all the operations about the
     * creation or deletion of collections during the test.
     */
    void
    validate(const std::string &operation_table_name, const std::string &schema_table_name,
      database &database)
    {
        WT_DECL_RET;
        wt_timestamp_t key_timestamp;
        std::vector<uint64_t> created_collections, deleted_collections;
        uint64_t key_collection_id;
        const char *key, *value;
        int value_operation_type;
        uint64_t collection_id = UINT64_MAX;

        scoped_session session = connection_manager::instance().create_session();

        /* Retrieve the collections that were created and deleted during the test. */
        parse_schema_tracking_table(
          session, schema_table_name, created_collections, deleted_collections);

        /*
         * Make sure the deleted collections do not exist on disk. The created collections are
         * checked in check_reference.
         */
        for (auto const &it : deleted_collections) {
            if (!verify_collection_state(session, it, false))
                testutil_die(DEBUG_ERROR,
                  "validate: collection %s present on disk while it has been tracked as deleted.",
                  database::build_collection_name(it).c_str());
        }

        /* Parse the tracking table. */
        scoped_cursor cursor = session.open_scoped_cursor(operation_table_name.c_str());
        while ((ret = cursor->next(cursor.get())) == 0) {
            testutil_check(cursor->get_key(cursor.get(), &key_collection_id, &key, &key_timestamp));
            testutil_check(cursor->get_value(cursor.get(), &value_operation_type, &value));

            debug_print("Collection id is " + std::to_string(key_collection_id), DEBUG_TRACE);
            debug_print("Key is " + std::string(key), DEBUG_TRACE);
            debug_print("Timestamp is " + std::to_string(key_timestamp), DEBUG_TRACE);
            debug_print("Operation type is " + std::to_string(value_operation_type), DEBUG_TRACE);
            debug_print("Value is " + std::string(value), DEBUG_TRACE);

            /*
             * If the cursor points to values from a collection that has been created during the
             * test, update the data model.
             */
            if (std::find(created_collections.begin(), created_collections.end(),
                  key_collection_id) != created_collections.end())
                update_data_model(static_cast<tracking_operation>(value_operation_type),
                  key_collection_id, key, value, database);
            /*
             * The collection should be part of the deleted collections if it has not be found in
             * the created ones.
             */
            else if (std::find(deleted_collections.begin(), deleted_collections.end(),
                       key_collection_id) == deleted_collections.end())
                testutil_die(DEBUG_ERROR,
                  "validate: The collection %s is not part of the created or deleted collections.",
                  key_collection_id);

            if (collection_id == UINT64_MAX)
                collection_id = key_collection_id;
            else if (collection_id != key_collection_id) {
                /*
                 * The data model is now fully updated for the last read collection. It can be
                 * checked.
                 */
                check_reference(session, collection_id, database);
                collection_id = key_collection_id;
            }
        };

        /* The value of ret should be WT_NOTFOUND once the cursor has read all rows. */
        if (ret != WT_NOTFOUND)
            testutil_die(DEBUG_ERROR, "validate: cursor->next() %d.", ret);

        /*
         * Once the cursor has read the entire table, the last parsed collection has not been
         * checked yet. We still have to make sure collection_id has been updated. It will remain
         * empty if there is no collections to check after the end of the test (no collections
         * created or all deleted).
         */
        if (collection_id != UINT64_MAX)
            check_reference(session, collection_id, database);
    }

    private:
    /*
     * Read the tracking table to retrieve the created and deleted collections during the test.
     * collection_name: collection that contains the operations on the different collections during
     * the test.
     */
    void
    parse_schema_tracking_table(scoped_session &session, const std::string &tracking_table_name,
      std::vector<uint64_t> &created_collections, std::vector<uint64_t> &deleted_collections)
    {
        wt_timestamp_t key_timestamp;
        uint64_t key_collection_id;
        int value_operation_type;

        scoped_cursor cursor = session.open_scoped_cursor(tracking_table_name.c_str());

        while (cursor->next(cursor.get()) == 0) {
            testutil_check(cursor->get_key(cursor.get(), &key_collection_id, &key_timestamp));
            testutil_check(cursor->get_value(cursor.get(), &value_operation_type));

            debug_print("Collection id is " + std::to_string(key_collection_id), DEBUG_TRACE);
            debug_print("Timestamp is " + std::to_string(key_timestamp), DEBUG_TRACE);
            debug_print("Operation type is " + std::to_string(value_operation_type), DEBUG_TRACE);

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

    /* Update the data model. */
    void
    update_data_model(const tracking_operation &operation, const uint64_t collection_id,
      const char *key, const char *value, database &database)
    {
        collection &collection = database.get_collection(collection_id);
        switch (operation) {
        case tracking_operation::DELETE_KEY:
            /*
             * Operations are parsed from the oldest to the most recent one. It is safe to assume
             * the key has been inserted previously in an existing collection and can be safely
             * deleted.
             */
            collection.delete_record(key);
            break;
        case tracking_operation::INSERT: {
            /*
             * Keys are unique, it is safe to assume the key has not been encountered before.
             */
            collection.insert_record(key, value);
            break;
        }
        case tracking_operation::UPDATE:
            collection.update_record(key, value);
            break;
        default:
            testutil_die(DEBUG_ERROR, "Unexpected operation in the tracking table: %d",
              static_cast<tracking_operation>(operation));
            break;
        }
    }

    /*
     * Compare the tracked operations against what has been saved on disk. collection:
     * representation in memory of the collection values and keys according to the tracking table.
     */
    void
    check_reference(scoped_session &session, const uint64_t collection_id, database &database)
    {
        bool is_valid;
        key_t key;
        key_value_t key_str;

        /* Check the collection exists on disk. */
        if (!verify_collection_state(session, collection_id, true))
            testutil_die(DEBUG_ERROR,
              "check_reference: collection %lu not present on disk while it has been tracked as "
              "created.",
              collection_id);

        collection &collection = database.get_collection(collection_id);

        /* Walk through each key/value pair of the current collection. */
        for (const auto &keys : collection.get_keys()) {
            key_str = keys.first;
            key = keys.second;
            /* The key/value pair exists. */
            if (key.exists)
                is_valid = (is_key_present(session, collection_id, key_str.c_str()) == true);
            /* The key has been deleted. */
            else
                is_valid = (is_key_present(session, collection_id, key_str.c_str()) == false);

            if (!is_valid)
                testutil_die(DEBUG_ERROR, "check_reference: failed for key %s in collection %lu.",
                  key_str.c_str(), collection_id);

            /* Check the associated value is valid. */
            if (key.exists) {
                if (!verify_value(session, collection_id, key_str.c_str(),
                      collection.get_record(key_str.c_str()).value))
                    testutil_die(DEBUG_ERROR,
                      "check_reference: failed for key %s / value %s in collection %lu.",
                      key_str.c_str(), collection.get_record(key_str.c_str()).value.c_str(),
                      collection_id);
            }
        }
    }

    /*
     * Check whether a collection exists on disk. exists: needs to be set to true if the collection
     * is expected to be existing, false otherwise.
     */
    bool
    verify_collection_state(
      scoped_session &session, const uint64_t collection_id, bool exists) const
    {
        /*
         * We don't necessarily expect to successfully open the cursor so don't create a scoped
         * cursor.
         */
        WT_CURSOR *cursor;
        int ret = session->open_cursor(session.get(),
          database::build_collection_name(collection_id).c_str(), nullptr, nullptr, &cursor);
        if (ret == 0)
            testutil_check(cursor->close(cursor));
        return (exists ? (ret == 0) : (ret != 0));
    }

    /* Check whether a keys exists in a collection on disk. */
    template <typename K>
    bool
    is_key_present(scoped_session &session, const uint64_t collection_id, const K &key)
    {
        scoped_cursor cursor =
          session.open_scoped_cursor(database::build_collection_name(collection_id).c_str());
        cursor->set_key(cursor.get(), key);
        return (cursor->search(cursor.get()) == 0);
    }

    /* Verify the given expected value is the same on disk. */
    template <typename K, typename V>
    bool
    verify_value(
      scoped_session &session, const uint64_t collection_id, const K &key, const V &expected_value)
    {
        const char *value;

        scoped_cursor cursor =
          session.open_scoped_cursor(database::build_collection_name(collection_id).c_str());
        cursor->set_key(cursor.get(), key);
        testutil_check(cursor->search(cursor.get()));
        testutil_check(cursor->get_value(cursor.get(), &value));

        return (key_value_t(value) == expected_value);
    }
};
} // namespace test_harness

#endif
