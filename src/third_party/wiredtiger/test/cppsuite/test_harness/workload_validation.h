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

namespace test_harness {
/*
 * Class that can validate database state and collection data.
 */
class workload_validation {
    public:
    /*
     * Validate the on disk data against what has been tracked during the test. The first step is to
     * replay the tracked operations so a representation in memory of the collections is created.
     * This representation is then compared to what is on disk. The second step is to go through
     * what has been saved on disk and make sure the memory representation has the same data.
     * operation_table_name is the collection that contains all the operations about the key/value
     * pairs in the different collections used during the test. schema_table_name is the collection
     * that contains all the operations about the creation or deletion of collections during the
     * test.
     */
    bool
    validate(const std::string &operation_table_name, const std::string &schema_table_name)
    {
        WT_SESSION *session;
        std::string collection_name;
        /*
         * Representation in memory of the collections at the end of the test. The first level is a
         * map that contains collection names as keys. The second level is another map that contains
         * the different key/value pairs within a given collection. If a collection yields to a null
         * map of key/value pairs, this means the collection should not be present on disk. If a
         * value from a key/value pair is null, this means the key should not be present in the
         * collection on disk.
         */
        std::map<std::string, std::map<int, std::string *> *> collections;
        /* Existing collections after the test. */
        std::vector<std::string> created_collections;
        bool is_valid;

        session = connection_manager::instance().create_session();

        /* Retrieve the created collections that need to be checked. */
        collection_name = schema_table_name;
        created_collections = parse_schema_tracking_table(session, collection_name);

        /* Allocate memory to the operations performed on the created collections. */
        for (auto const &it : created_collections) {
            std::map<int, std::string *> *map = new std::map<int, std::string *>();
            collections[it] = map;
        }

        /*
         * Build in memory the final state of each created collection according to the tracked
         * operations.
         */
        collection_name = operation_table_name;
        for (auto const &active_collection : created_collections)
            parse_operation_tracking_table(
              session, collection_name, active_collection, collections);

        /* Check all tracked operations in memory against the database on disk. */
        is_valid = check_reference(session, collections);

        /* Check what has been saved on disk against what has been tracked. */
        if (is_valid) {
            for (auto const &collection : created_collections) {
                is_valid = check_disk_state(session, collection, collections);
                if (!is_valid) {
                    debug_print(
                      "check_disk_state failed for collection " + collection, DEBUG_ERROR);
                    break;
                }
            }

        } else
            debug_print("check_reference failed!", DEBUG_ERROR);

        /* Clean the allocated memory. */
        clean_memory(collections);

        return (is_valid);
    }

    /* Clean the memory used to represent the collections after the test. */
    void
    clean_memory(std::map<std::string, std::map<int, std::string *> *> &collections)
    {
        for (auto &it_collections : collections) {
            if (it_collections.second == nullptr)
                continue;

            for (auto &it_operations : *it_collections.second) {
                delete it_operations.second;
                it_operations.second = nullptr;
            }
            delete it_collections.second;
            it_collections.second = nullptr;
        }
    }

    /*
     * collection_name is the collection that contains the operations on the different collections
     * during the test.
     */
    const std::vector<std::string>
    parse_schema_tracking_table(WT_SESSION *session, const std::string &collection_name)
    {
        WT_CURSOR *cursor;
        const char *key_collection_name;
        int key_timestamp, value_operation_type;
        std::vector<std::string> created_collections;

        testutil_check(session->open_cursor(session, collection_name.c_str(), NULL, NULL, &cursor));

        while (cursor->next(cursor) == 0) {
            testutil_check(cursor->get_key(cursor, &key_collection_name, &key_timestamp));
            testutil_check(cursor->get_value(cursor, &value_operation_type));

            debug_print("Collection name is " + std::string(key_collection_name), DEBUG_TRACE);
            debug_print("Timestamp is " + std::to_string(key_timestamp), DEBUG_TRACE);
            debug_print("Operation type is " + std::to_string(value_operation_type), DEBUG_TRACE);

            if (static_cast<tracking_operation>(value_operation_type) ==
              tracking_operation::CREATE) {
                created_collections.push_back(key_collection_name);
            } else if (static_cast<tracking_operation>(value_operation_type) ==
              tracking_operation::DELETE_COLLECTION) {
                created_collections.erase(std::remove(created_collections.begin(),
                                            created_collections.end(), key_collection_name),
                  created_collections.end());
            }
        }

        return (created_collections);
    }

    /*
     * Parse the tracked operations to build a representation in memory of the collections at the
     * end of the test. tracking_collection_name is the tracking collection used to save the
     * operations performed on the collections during the test. collection_name is the collection
     * that needs to be represented in memory.
     */
    void
    parse_operation_tracking_table(WT_SESSION *session, const std::string &tracking_collection_name,
      const std::string &collection_name,
      std::map<std::string, std::map<int, std::string *> *> &collections)
    {
        WT_CURSOR *cursor;
        int error_code, exact, key, key_timestamp, value_operation_type;
        const char *key_collection_name, *value;

        testutil_check(
          session->open_cursor(session, tracking_collection_name.c_str(), NULL, NULL, &cursor));

        /* Our keys start at 0. */
        cursor->set_key(cursor, collection_name.c_str(), 0);
        error_code = cursor->search_near(cursor, &exact);

        /*
         * As we don't support deletion, the searched collection is expected to be found. Since the
         * timestamp which is part of the key is not provided, exact is expected to be > 0.
         */
        testutil_check(exact < 1);

        while (error_code == 0) {
            testutil_check(cursor->get_key(cursor, &key_collection_name, &key, &key_timestamp));
            testutil_check(cursor->get_value(cursor, &value_operation_type, &value));

            debug_print("Collection name is " + std::string(key_collection_name), DEBUG_TRACE);
            debug_print("Key is " + std::to_string(key), DEBUG_TRACE);
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
                 * deleted safely.
                 */
                delete collections.at(key_collection_name)->at(key);
                collections.at(key_collection_name)->at(key) = nullptr;
                break;
            case tracking_operation::INSERT: {
                /* Keys are unique, it is safe to assume the key has not been encountered before. */
                std::pair<int, std::string *> pair(key, new std::string(value));
                collections.at(key_collection_name)->insert(pair);
                break;
            }
            case tracking_operation::CREATE:
            case tracking_operation::DELETE_COLLECTION:
                testutil_die(DEBUG_ABORT, "Unexpected operation in the tracking table: %d",
                  static_cast<tracking_operation>(value_operation_type));
            default:
                testutil_die(
                  DEBUG_ABORT, "tracking operation is unknown : %d", value_operation_type);
                break;
            }

            error_code = cursor->next(cursor);
        }

        if (cursor->reset(cursor) != 0)
            debug_print("Cursor could not be reset !", DEBUG_ERROR);
    }

    /*
     * Compare the tracked operations against what has been saved on disk. collections is the
     * representation in memory of the collections after the test according to the tracking table.
     */
    bool
    check_reference(
      WT_SESSION *session, std::map<std::string, std::map<int, std::string *> *> &collections)
    {

        bool collection_exists, is_valid = true;
        std::map<int, std::string *> *collection;
        workload_validation wv;
        std::string *value;

        for (const auto &it_collections : collections) {
            /* Check the collection is in the correct state. */
            collection_exists = (it_collections.second != nullptr);
            is_valid = wv.verify_database_state(session, it_collections.first, collection_exists);

            if (is_valid && collection_exists) {
                collection = it_collections.second;
                for (const auto &it_operations : *collection) {
                    value = (*collection)[it_operations.first];
                    /* The key/value pair exists. */
                    if (value != nullptr)
                        is_valid = (wv.is_key_present(
                                      session, it_collections.first, it_operations.first) == true);
                    /* The key has been deleted. */
                    else
                        is_valid = (wv.is_key_present(
                                      session, it_collections.first, it_operations.first) == false);

                    /* Check the associated value is valid. */
                    if (is_valid && (value != nullptr)) {
                        is_valid = (wv.verify_value(
                          session, it_collections.first, it_operations.first, *value));
                    }

                    if (!is_valid) {
                        debug_print(
                          "check_reference failed for key " + std::to_string(it_operations.first),
                          DEBUG_ERROR);
                        break;
                    }
                }
            }

            if (!is_valid) {
                debug_print(
                  "check_reference failed for collection " + it_collections.first, DEBUG_ERROR);
                break;
            }
        }

        return (is_valid);
    }

    /* Check what is present on disk against what has been tracked. */
    bool
    check_disk_state(WT_SESSION *session, const std::string &collection_name,
      std::map<std::string, std::map<int, std::string *> *> &collections)
    {
        WT_CURSOR *cursor;
        int key;
        const char *value;
        bool is_valid;
        std::string *value_str;
        std::map<int, std::string *> *collection;

        testutil_check(session->open_cursor(session, collection_name.c_str(), NULL, NULL, &cursor));

        /* Check the collection has been tracked and contains data. */
        is_valid =
          ((collections.count(collection_name) > 0) && (collections[collection_name] != nullptr));

        if (!is_valid)
            debug_print(
              "Collection " + collection_name + " has not been tracked or has been deleted",
              DEBUG_ERROR);
        else
            collection = collections[collection_name];

        /* Read the collection on disk. */
        while (is_valid && (cursor->next(cursor) == 0)) {
            testutil_check(cursor->get_key(cursor, &key));
            testutil_check(cursor->get_value(cursor, &value));

            debug_print("Key is " + std::to_string(key), DEBUG_TRACE);
            debug_print("Value is " + std::string(value), DEBUG_TRACE);

            if (collection->count(key) > 0) {
                value_str = collection->at(key);
                /*
                 * Check the key/value pair on disk matches the one in memory from the tracked
                 * operations.
                 */
                is_valid = (value_str != nullptr) && (*value_str == std::string(value));
                if (!is_valid)
                    debug_print(" Key/Value pair mismatch.\n Disk key: " + std::to_string(key) +
                        "\n Disk value: " + std ::string(value) +
                        "\n Tracking table key: " + std::to_string(key) +
                        "\n Tracking table value: " + (value_str == nullptr ? "NULL" : *value_str),
                      DEBUG_ERROR);
            } else {
                is_valid = false;
                debug_print(
                  "The key " + std::to_string(key) + " present on disk has not been tracked",
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
    verify_database_state(
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

        return (value == expected_value);
    }

    private:
};
} // namespace test_harness

#endif
