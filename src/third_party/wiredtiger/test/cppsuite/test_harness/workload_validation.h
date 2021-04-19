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
     * Validate the on disk data against what has been tracked during the test.
     * - The first step is to replay the tracked operations so a representation in memory of the
     * collections is created. This representation is then compared to what is on disk.
     * - The second step is to go through what has been saved on disk and make sure the memory
     * representation has the same data.
     * operation_table_name is the collection that contains all the operations about the key/value
     * pairs in the different collections used during the test. schema_table_name is the collection
     * that contains all the operations about the creation or deletion of collections during the
     * test.
     */
    bool
    validate(const std::string &operation_table_name, const std::string &schema_table_name,
      database &database)
    {
        WT_SESSION *session;
        std::string collection_name;
        /* Existing collections after the test. */
        std::vector<std::string> created_collections, deleted_collections;
        bool is_valid = true;

        session = connection_manager::instance().create_session();

        /* Retrieve the collections that were created and deleted during the test. */
        collection_name = schema_table_name;
        parse_schema_tracking_table(
          session, collection_name, created_collections, deleted_collections);

        /* Make sure they exist in memory. */
        for (auto const &it : created_collections) {
            if (database.collections.count(it) == 0) {
                debug_print("Collection missing in memory: " + it, DEBUG_ERROR);
                is_valid = false;
                break;
            }
        }

        if (!is_valid)
            return (is_valid);

        /* Make sure they don't exist in memory nor on disk. */
        for (auto const &it : deleted_collections) {
            if (database.collections.count(it) > 0) {
                debug_print(
                  "Collection present in memory while it has been tracked as deleted: " + it,
                  DEBUG_ERROR);
                is_valid = false;
                break;
            }
            if (!verify_collection_state(session, it, false)) {
                debug_print(
                  "Collection present on disk while it has been tracked as deleted: " + it,
                  DEBUG_ERROR);
                is_valid = false;
                break;
            }
        }

        for (auto const &collection_name : database.get_collection_names()) {
            if (!is_valid)
                break;

            /* Get the values associated to the different keys in the current collection. */
            parse_operation_tracking_table(
              session, operation_table_name, collection_name, database);
            /* Check all tracked operations in memory against the database on disk. */
            if (!check_reference(session, collection_name, database)) {
                debug_print(
                  "check_reference failed for collection " + collection_name, DEBUG_ERROR);
                is_valid = false;
            }
            /* Check what has been saved on disk against what has been tracked. */
            else if (!check_disk_state(session, collection_name, database)) {
                debug_print(
                  "check_disk_state failed for collection " + collection_name, DEBUG_ERROR);
                is_valid = false;
            }
            /* Clear memory. */
            delete database.collections[collection_name].values;
            database.collections[collection_name].values = nullptr;
        }

        return (is_valid);
    }

    private:
    /*
     * Read the tracking table to retrieve the created and deleted collections during the test.
     * collection_name is the collection that contains the operations on the different collections
     * during the test.
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
              tracking_operation::CREATE) {
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

    /*
     * Parse the tracked operations to build a representation in memory of the collections at the
     * end of the test. tracking_collection_name is the tracking collection used to save the
     * operations performed on the collections during the test. collection_name is the collection
     * that needs to be represented in memory.
     */
    void
    parse_operation_tracking_table(WT_SESSION *session, const std::string &tracking_collection_name,
      const std::string &collection_name, database &database)
    {
        WT_CURSOR *cursor;
        wt_timestamp_t key_timestamp;
        int exact, value_operation_type;
        const char *key, *key_collection_name, *value;
        std::vector<key_value_t> collection_keys;
        std::string key_str;

        /* Retrieve all keys from the given collection. */
        for (auto const &it : database.collections.at(collection_name).keys)
            collection_keys.push_back(it.first);
        /* There must be at least a key. */
        testutil_assert(!collection_keys.empty());
        /* Sort keys. */
        std::sort(collection_keys.begin(), collection_keys.end());
        /* Use the first key as a parameter for search_near. */
        key_str = collection_keys[0];

        testutil_check(
          session->open_cursor(session, tracking_collection_name.c_str(), NULL, NULL, &cursor));

        cursor->set_key(cursor, collection_name.c_str(), key_str.c_str());
        testutil_check(cursor->search_near(cursor, &exact));
        /*
         * Since the timestamp which is part of the key is not provided, exact is expected to be
         * greater than 0.
         */
        testutil_assert(exact >= 0);

        do {
            testutil_check(cursor->get_key(cursor, &key_collection_name, &key, &key_timestamp));
            testutil_check(cursor->get_value(cursor, &value_operation_type, &value));

            debug_print("Collection name is " + std::string(key_collection_name), DEBUG_TRACE);
            debug_print("Key is " + std::string(key), DEBUG_TRACE);
            debug_print("Timestamp is " + std::to_string(key_timestamp), DEBUG_TRACE);
            debug_print("Operation type is " + std::to_string(value_operation_type), DEBUG_TRACE);
            debug_print("Value is " + std::string(value), DEBUG_TRACE);

            /*
             * If the cursor is reading an operation for a different collection, we know all the
             * operations have been parsed for the collection we were interested in.
             */
            if (std::string(key_collection_name) != collection_name)
                break;

            /* Replay the current operation. */
            switch (static_cast<tracking_operation>(value_operation_type)) {
            case tracking_operation::DELETE_KEY:
                /*
                 * Operations are parsed from the oldest to the most recent one. It is safe to
                 * assume the key has been inserted previously in an existing collection and can be
                 * safely deleted.
                 */
                database.collections.at(key_collection_name).keys.at(std::string(key)).exists =
                  false;
                delete database.collections.at(key_collection_name).values;
                database.collections.at(key_collection_name).values = nullptr;
                break;
            case tracking_operation::INSERT: {
                /* Keys are unique, it is safe to assume the key has not been encountered before. */
                database.collections[key_collection_name].keys[std::string(key)].exists = true;
                if (database.collections[key_collection_name].values == nullptr) {
                    database.collections[key_collection_name].values =
                      new std::map<key_value_t, value_t>();
                }
                value_t v;
                v.value = key_value_t(value);
                std::pair<key_value_t, value_t> pair(key_value_t(key), v);
                database.collections[key_collection_name].values->insert(pair);
                break;
            }
            case tracking_operation::UPDATE:
                database.collections[key_collection_name].values->at(key).value =
                  key_value_t(value);
                break;
            default:
                testutil_die(DEBUG_ABORT, "Unexpected operation in the tracking table: %d",
                  value_operation_type);
                break;
            }

        } while (cursor->next(cursor) == 0);

        if (cursor->reset(cursor) != 0)
            debug_print("Cursor could not be reset !", DEBUG_ERROR);
    }

    /*
     * Compare the tracked operations against what has been saved on disk. database is the
     * representation in memory of the collections after the test according to the tracking table.
     */
    bool
    check_reference(
      WT_SESSION *session, const std::string &collection_name, const database &database)
    {
        bool is_valid;
        collection_t collection;
        key_t key;
        key_value_t key_str;

        /* Check the collection exists on disk. */
        is_valid = verify_collection_state(session, collection_name, true);

        if (is_valid) {
            collection = database.collections.at(collection_name);
            /* Walk through each key/value pair of the current collection. */
            for (const auto &keys : collection.keys) {
                key_str = keys.first;
                key = keys.second;
                /* The key/value pair exists. */
                if (key.exists)
                    is_valid = (is_key_present(session, collection_name, key_str.c_str()) == true);
                /* The key has been deleted. */
                else
                    is_valid = (is_key_present(session, collection_name, key_str.c_str()) == false);

                /* Check the associated value is valid. */
                if (is_valid && key.exists) {
                    testutil_assert(collection.values != nullptr);
                    is_valid = verify_value(session, collection_name, key_str.c_str(),
                      collection.values->at(key_str).value);
                }

                if (!is_valid) {
                    debug_print("check_reference failed for key " + key_str, DEBUG_ERROR);
                    break;
                }
            }
        }

        if (!is_valid)
            debug_print("check_reference failed for collection " + collection_name, DEBUG_ERROR);

        return (is_valid);
    }

    /* Check what is present on disk against what has been tracked. */
    bool
    check_disk_state(
      WT_SESSION *session, const std::string &collection_name, const database &database)
    {
        WT_CURSOR *cursor;
        collection_t collection;
        bool is_valid = true;
        /* Key/value pairs on disk. */
        const char *key_on_disk, *value_on_disk;
        key_value_t key_str, value_str;

        testutil_check(session->open_cursor(session, collection_name.c_str(), NULL, NULL, &cursor));

        collection = database.collections.at(collection_name);

        /* Read the collection on disk. */
        while (is_valid && (cursor->next(cursor) == 0)) {
            testutil_check(cursor->get_key(cursor, &key_on_disk));
            testutil_check(cursor->get_value(cursor, &value_on_disk));

            key_str = std::string(key_on_disk);

            debug_print("Key on disk is " + key_str, DEBUG_TRACE);
            debug_print("Value on disk is " + std::string(value_on_disk), DEBUG_TRACE);

            /* Check the key on disk has been saved in memory too. */
            if ((collection.keys.count(key_str) > 0) && collection.keys.at(key_str).exists) {
                /* Memory should be allocated for values. */
                testutil_assert(collection.values != nullptr);
                value_str = collection.values->at(key_str).value;
                /*
                 * Check the key/value pair on disk matches the one in memory from the tracked
                 * operations.
                 */
                is_valid = (value_str == key_value_t(value_on_disk));
                if (!is_valid)
                    debug_print(" Key/Value pair mismatch.\n Disk key: " + key_str +
                        "\n Disk value: " + std ::string(value_on_disk) +
                        "\n Tracking table key: " + key_str + "\n Tracking table value exists: " +
                        std::to_string(collection.keys.at(key_str).exists) +
                        "\n Tracking table value: " + value_str,
                      DEBUG_ERROR);
            } else {
                is_valid = false;
                debug_print(
                  "The key " + std::string(key_on_disk) + " present on disk has not been tracked",
                  DEBUG_ERROR);
            }
        }

        return (is_valid);
    }

    /*
     * Check whether a collection exists on disk. collection_name is the collection to check. exists
     * needs to be set to true if the collection is expected to be existing, false otherwise.
     */
    bool
    verify_collection_state(
      WT_SESSION *session, const std::string &collection_name, bool exists) const
    {
        WT_CURSOR *cursor;
        int ret = session->open_cursor(session, collection_name.c_str(), NULL, NULL, &cursor);
        return (exists ? (ret == 0) : (ret != 0));
    }

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
