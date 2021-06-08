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
        WT_CURSOR *cursor;
        WT_SESSION *session;
        wt_timestamp_t key_timestamp;
        std::vector<std::string> created_collections, deleted_collections;
        const char *key, *key_collection_name, *value;
        int value_operation_type;
        std::string collection_name;

        session = connection_manager::instance().create_session();

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
                  it.c_str());
        }

        /* Parse the tracking table. */
        testutil_check(
          session->open_cursor(session, operation_table_name.c_str(), NULL, NULL, &cursor));
        while ((ret = cursor->next(cursor)) == 0) {
            testutil_check(cursor->get_key(cursor, &key_collection_name, &key, &key_timestamp));
            testutil_check(cursor->get_value(cursor, &value_operation_type, &value));

            debug_print("Collection name is " + std::string(key_collection_name), DEBUG_TRACE);
            debug_print("Key is " + std::string(key), DEBUG_TRACE);
            debug_print("Timestamp is " + std::to_string(key_timestamp), DEBUG_TRACE);
            debug_print("Operation type is " + std::to_string(value_operation_type), DEBUG_TRACE);
            debug_print("Value is " + std::string(value), DEBUG_TRACE);

            /*
             * If the cursor points to values from a collection that has been created during the
             * test, update the data model.
             */
            if (std::find(created_collections.begin(), created_collections.end(),
                  key_collection_name) != created_collections.end())
                update_data_model(static_cast<tracking_operation>(value_operation_type),
                  key_collection_name, key, value, database);
            /*
             * The collection should be part of the deleted collections if it has not be found in
             * the created ones.
             */
            else if (std::find(deleted_collections.begin(), deleted_collections.end(),
                       key_collection_name) == deleted_collections.end())
                testutil_die(DEBUG_ERROR,
                  "validate: The collection %s is not part of the created or deleted collections.",
                  key_collection_name);

            if (collection_name.empty())
                collection_name = key_collection_name;
            else if (collection_name != key_collection_name) {
                /*
                 * The data model is now fully updated for the last read collection. It can be
                 * checked.
                 */
                check_reference(session, collection_name, database);
                collection_name = key_collection_name;
            }
        };

        /* The value of ret should be WT_NOTFOUND once the cursor has read all rows. */
        if (ret != WT_NOTFOUND)
            testutil_die(DEBUG_ERROR, "validate: cursor->next() %d.", ret);

        /*
         * Once the cursor has read the entire table, the last parsed collection has not been
         * checked yet. We still have to make sure collection_name has been updated. It will remain
         * empty if there is no collections to check after the end of the test (no collections
         * created or all deleted).
         */
        if (!collection_name.empty())
            check_reference(session, collection_name, database);
    }

    private:
    /*
     * Read the tracking table to retrieve the created and deleted collections during the test.
     * collection_name: collection that contains the operations on the different collections during
     * the test.
     */
    void
    parse_schema_tracking_table(WT_SESSION *session, const std::string &collection_name,
      std::vector<std::string> &created_collections, std::vector<std::string> &deleted_collections)
    {
        WT_CURSOR *cursor;
        wt_timestamp_t key_timestamp;
        const char *key_collection_name;
        int value_operation_type;

        testutil_check(session->open_cursor(session, collection_name.c_str(), NULL, NULL, &cursor));

        while (cursor->next(cursor) == 0) {
            testutil_check(cursor->get_key(cursor, &key_collection_name, &key_timestamp));
            testutil_check(cursor->get_value(cursor, &value_operation_type));

            debug_print("Collection name is " + std::string(key_collection_name), DEBUG_TRACE);
            debug_print("Timestamp is " + std::to_string(key_timestamp), DEBUG_TRACE);
            debug_print("Operation type is " + std::to_string(value_operation_type), DEBUG_TRACE);

            if (static_cast<tracking_operation>(value_operation_type) ==
              tracking_operation::CREATE_COLLECTION) {
                deleted_collections.erase(std::remove(deleted_collections.begin(),
                                            deleted_collections.end(), key_collection_name),
                  deleted_collections.end());
                created_collections.push_back(key_collection_name);
            } else if (static_cast<tracking_operation>(value_operation_type) ==
              tracking_operation::DELETE_COLLECTION) {
                created_collections.erase(std::remove(created_collections.begin(),
                                            created_collections.end(), key_collection_name),
                  created_collections.end());
                deleted_collections.push_back(key_collection_name);
            }
        }
    }

    /* Update the data model. */
    void
    update_data_model(const tracking_operation &operation, const std::string &collection_name,
      const char *key, const char *value, database &database)
    {
        switch (operation) {
        case tracking_operation::DELETE_KEY:
            /*
             * Operations are parsed from the oldest to the most recent one. It is safe to assume
             * the key has been inserted previously in an existing collection and can be safely
             * deleted.
             */
            database.delete_record(collection_name, key);
            break;
        case tracking_operation::INSERT: {
            /*
             * Keys are unique, it is safe to assume the key has not been encountered before.
             */
            database.insert_record(collection_name, key, value);
            break;
        }
        case tracking_operation::UPDATE:
            database.update_record(collection_name, key, value);
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
    check_reference(WT_SESSION *session, const std::string &collection_name, database &database)
    {
        bool is_valid;
        key_t key;
        key_value_t key_str;

        /* Check the collection exists on disk. */
        if (!verify_collection_state(session, collection_name, true))
            testutil_die(DEBUG_ERROR,
              "check_reference: collection %s not present on disk while it has been tracked as "
              "created.",
              collection_name.c_str());

        /* Walk through each key/value pair of the current collection. */
        for (const auto &keys : database.get_keys(collection_name)) {
            key_str = keys.first;
            key = keys.second;
            /* The key/value pair exists. */
            if (key.exists)
                is_valid = (is_key_present(session, collection_name, key_str.c_str()) == true);
            /* The key has been deleted. */
            else
                is_valid = (is_key_present(session, collection_name, key_str.c_str()) == false);

            if (!is_valid)
                testutil_die(DEBUG_ERROR, "check_reference: failed for key %s in collection %s.",
                  key_str.c_str(), collection_name.c_str());

            /* Check the associated value is valid. */
            if (key.exists) {
                if (!verify_value(session, collection_name, key_str.c_str(),
                      database.get_record(collection_name, key_str.c_str()).value))
                    testutil_die(DEBUG_ERROR,
                      "check_reference: failed for key %s / value %s in collection %s.",
                      key_str.c_str(),
                      database.get_record(collection_name, key_str.c_str()).value.c_str(),
                      collection_name.c_str());
            }
        }
    }

    /*
     * Check whether a collection exists on disk. exists: needs to be set to true if the collection
     * is expected to be existing, false otherwise.
     */
    bool
    verify_collection_state(
      WT_SESSION *session, const std::string &collection_name, bool exists) const
    {
        WT_CURSOR *cursor;
        int ret = session->open_cursor(session, collection_name.c_str(), NULL, NULL, &cursor);
        return (exists ? (ret == 0) : (ret != 0));
    }

    /* Check whether a keys exists in a collection on disk. */
    template <typename K>
    bool
    is_key_present(WT_SESSION *session, const std::string &collection_name, const K &key)
    {
        WT_CURSOR *cursor;
        testutil_check(session->open_cursor(session, collection_name.c_str(), NULL, NULL, &cursor));
        cursor->set_key(cursor, key);
        return (cursor->search(cursor) == 0);
    }

    /* Verify the given expected value is the same on disk. */
    template <typename K, typename V>
    bool
    verify_value(WT_SESSION *session, const std::string &collection_name, const K &key,
      const V &expected_value)
    {
        WT_CURSOR *cursor;
        const char *value;

        testutil_check(session->open_cursor(session, collection_name.c_str(), NULL, NULL, &cursor));
        cursor->set_key(cursor, key);
        testutil_check(cursor->search(cursor));
        testutil_check(cursor->get_value(cursor, &value));

        return (key_value_t(value) == expected_value);
    }
};
} // namespace test_harness

#endif
