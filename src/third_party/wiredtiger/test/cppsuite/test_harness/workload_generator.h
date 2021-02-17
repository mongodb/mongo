/* Include guard. */
#ifndef WORKLOAD_GENERATOR_H
#define WORKLOAD_GENERATOR_H

#include <cstdint>
#include <map>
#include <vector>

extern "C" {
#include "test_util.h"
}

#include "api_const.h"
#include "configuration_settings.h"
#include "random_generator.h"

#define DEBUG_ERROR 0
#define DEBUG_INFO 1

namespace test_harness {
class workload_generator {
    public:
    workload_generator(test_harness::configuration *configuration)
    {
        _configuration = configuration;
    }

    ~workload_generator()
    {
        if (_session != nullptr) {
            if (_session->close(_session, NULL) != 0)
                /* Failing to close session is not blocking. */
                debug_info("Failed to close session, shutting down uncleanly", DEBUG_ERROR);
            _session = nullptr;
        }

        if (_conn != nullptr) {
            if (_conn->close(_conn, NULL) != 0)
                /* Failing to close connection is not blocking. */
                debug_info("Failed to close connection, shutting down uncleanly", DEBUG_ERROR);
            _conn = nullptr;
        }
    }

    /*
     * Function that performs the following steps using the configuration that is defined by the
     * test:
     *  - Create the working dir.
     *  - Open a connection.
     *  - Open a session.
     *  - Create n collections as per the configuration.
     *      - Open a cursor on each collection.
     *      - Insert m key/value pairs in each collection. Values are random strings which size is
     * defined by the configuration.
     */
    int
    load(const char *home = DEFAULT_DIR)
    {
        WT_CURSOR *cursor;
        int64_t collection_count, key_count, value_size;
        std::string collection_name;

        cursor = nullptr;
        collection_count = key_count = value_size = 0;
        collection_name = "";

        /* Create the working dir. */
        testutil_make_work_dir(home);

        /* Open connection. */
        WT_RET(wiredtiger_open(home, NULL, test_harness::CONNECTION_CREATE, &_conn));

        /* Open session. */
        WT_RET(_conn->open_session(_conn, NULL, NULL, &_session));

        /* Create n collections as per the configuration and store each collection name. */
        WT_RET(_configuration->get_int(test_harness::COLLECTION_COUNT, collection_count));
        for (int i = 0; i < collection_count; ++i) {
            collection_name = "table:collection" + std::to_string(i);
            WT_RET(_session->create(_session, collection_name.c_str(), DEFAULT_TABLE_SCHEMA));
            _collection_names.push_back(collection_name);
        }
        debug_info(std::to_string(collection_count) + " collections created", DEBUG_INFO);

        /* Open a cursor on each collection and use the configuration to insert key/value pairs. */
        WT_RET(_configuration->get_int(test_harness::KEY_COUNT, key_count));
        WT_RET(_configuration->get_int(test_harness::VALUE_SIZE, value_size));
        for (const auto &collection_name : _collection_names) {
            /* WiredTiger lets you open a cursor on a collection using the same pointer. When a
             * session is closed, WiredTiger APIs close the cursors too. */
            WT_RET(_session->open_cursor(_session, collection_name.c_str(), NULL, NULL, &cursor));
            for (size_t j = 0; j < key_count; ++j) {
                cursor->set_key(cursor, j);
                /* Generation of a random string value using the size defined in the test
                 * configuration. */
                std::string generated_value =
                  random_generator::random_generator::get_instance()->generate_string(value_size);
                cursor->set_value(cursor, generated_value.c_str());
                WT_RET(cursor->insert(cursor));
            }
        }
        debug_info(std::to_string(collection_count) + " key/value inserted", DEBUG_INFO);
        return (0);
    }

    /* Do the work of the main part of the workload. */
    int
    run()
    {
        /* Empty until thread management lib is implemented. */
        return (0);
    }

    /* WiredTiger APIs wrappers for single operations. */
    int
    insert(WT_CURSOR *cursor, const char *value)
    {
        if (cursor == nullptr)
            throw std::invalid_argument("Failed to call insert, invalid cursor");
        return (cursor->insert(cursor));
    }

    int
    search(WT_CURSOR *cursor)
    {
        if (cursor == nullptr)
            throw std::invalid_argument("Failed to call search, invalid cursor");
        return (cursor->search(cursor));
    }

    int
    search_near(WT_CURSOR *cursor, int *exact)
    {
        if (cursor == nullptr)
            throw std::invalid_argument("Failed to call search_near, invalid cursor");
        return (cursor->search_near(cursor, exact));
    }

    int
    update(WT_CURSOR *cursor)
    {
        if (cursor == nullptr)
            throw std::invalid_argument("Failed to call update, invalid cursor");
        return (cursor->update(cursor));
    }

    /* Trace level that is set in the test configuration for debugging purpose. */
    static int64_t _trace_level;

    private:
    /* Used to print out traces for debugging purpose. */
    static void
    debug_info(const std::string &str, int64_t trace_level)
    {
        if (_trace_level >= trace_level)
            std::cout << str << std::endl;
    }

    std::vector<std::string> _collection_names;
    test_harness::configuration *_configuration = nullptr;
    WT_CONNECTION *_conn = nullptr;
    WT_SESSION *_session = nullptr;
};
} // namespace test_harness

#endif
