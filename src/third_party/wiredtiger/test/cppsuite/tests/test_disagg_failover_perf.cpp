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

/*
 * [test_disagg_failover_perf]: Measure how long disagg failover takes on a running system.
 */

#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/common/random_generator.h"
#include "src/common/thread_manager.h"
#include "src/storage/connection_manager.h"
#include "src/storage/scoped_session.h"
#include "src/main/database.h"
#include "src/main/database_operation.h"
#include "src/main/crud.h"
#include "src/component/metrics_monitor.h"
#include "src/component/metrics_writer.h"

extern "C" {
#include "wiredtiger.h"
#include "test_util.h"
}

/* Declare getopt external variables. */
extern "C" {
extern int __wt_optind;
extern char *__wt_optarg;
}

#include <iostream>
#include <map>
#include <sstream>
#include <string>

enum class workload_type { append, update };

using namespace test_harness;
struct options {
    int collection_count = 3;
    int cache_size_gb = 16;
    int key_count = 5000;
    int key_size = 10;
    int value_size = 1000;
    int ingest_size_mb = 1;
    int verbose_level = 0;
    workload_type type = workload_type::update;
    std::string home_path = DEFAULT_DIR;
    int warm_cache_pct = 0;
    bool load_skip = false;
    bool load_copy = false;
};

options opt;
wt_timestamp_t ts = 100;
test_harness::database *database_model;

/* Setup the database model. */
static void
initialize()
{
    database_model = new database();
    database_model->set_create_config(false, false, true);
}

/* Generate a key. */
static std::string
generate_key(int key)
{
    return thread_worker::pad_string(std::to_string(key), opt.key_size);
}

/* Generate a value. */
static std::string
generate_value()
{
    return random_generator::instance().generate_pseudo_random_string(opt.value_size);
}

static bool
parse_options(int argc, char *argv[], options &out, std::string &error)
{
    int ch;
    const char *shape = nullptr;

    while ((ch = __wt_getopt("test_disagg_failover_perf", argc, argv, "c:g:h:i:k:s:v:w:CLS:V:")) !=
      EOF) {
        switch (ch) {
        case 'c':
            out.collection_count = atoi(__wt_optarg);
            if (out.collection_count <= 0) {
                error = "invalid collection_count: " + std::string(__wt_optarg);
                return false;
            }
            break;
        case 'g':
            out.cache_size_gb = atoi(__wt_optarg);
            if (out.cache_size_gb <= 0) {
                error = "invalid cache_size_gb: " + std::string(__wt_optarg);
                return false;
            }
            break;
        case 'h':
            out.home_path = __wt_optarg;
            break;
        case 'i':
            out.ingest_size_mb = atoi(__wt_optarg);
            if (out.ingest_size_mb < 0) {
                error = "invalid ingest_size_mb: " + std::string(__wt_optarg);
                return false;
            }
            break;
        case 'k':
            out.key_count = atoi(__wt_optarg);
            if (out.key_count <= 0) {
                error = "invalid key_count: " + std::string(__wt_optarg);
                return false;
            }
            break;
        case 's':
            out.key_size = atoi(__wt_optarg);
            if (out.key_size <= 0) {
                error = "invalid key_size: " + std::string(__wt_optarg);
                return false;
            }
            break;
        case 'v':
            out.value_size = atoi(__wt_optarg);
            if (out.value_size <= 0) {
                error = "invalid value_size: " + std::string(__wt_optarg);
                return false;
            }
            break;
        case 'w':
            out.warm_cache_pct = atoi(__wt_optarg);
            if (out.warm_cache_pct < 0 || out.warm_cache_pct > 100) {
                error = "invalid warm_cache_pct (must be 0-100): " + std::string(__wt_optarg);
                return false;
            }
            break;
        case 'C':
            out.load_copy = true;
            break;
        case 'L':
            out.load_skip = true;
            break;
        case 'S':
            shape = __wt_optarg;
            if (strcmp(shape, "append") == 0)
                out.type = workload_type::append;
            else if (strcmp(shape, "updates") == 0)
                out.type = workload_type::update;
            else {
                error =
                  "invalid workload_shape (expected 'append' or 'updates'): " + std::string(shape);
                return false;
            }
            break;
        case 'V':
            out.verbose_level = atoi(__wt_optarg);
            if (out.verbose_level < 0) {
                error = "invalid verbose_level: " + std::string(__wt_optarg);
                return false;
            }
            break;
        case '?':
        default:
            error = "unknown option";
            return false;
        }
    }
    return true;
}

static void
update_global_timestamps()
{
    std::string config;
    config += STABLE_TS + "=" + timestamp_manager::decimal_to_hex(++ts) + ",";
    config += OLDEST_TS + "=" + timestamp_manager::decimal_to_hex(ts - 20);
    logger::log_msg(LOG_TRACE, "Updating global timestamps " + config);
    connection_manager::instance().set_timestamp(config);
}

static void
populate()
{
    logger::log_msg(LOG_INFO,
      "Populate: Starting creation of " + std::to_string(opt.collection_count) + " collections.");

    /* Create n collections as per the configuration. */
    scoped_session session = connection_manager::instance().create_session();
    for (int64_t i = 0; i < opt.collection_count; ++i) {
        logger::log_msg(LOG_INFO,
          "Populate: Creating collection " + std::to_string(i + 1) + "/" +
            std::to_string(opt.collection_count));
        /*
         * The database model will call into the API and create the collection, with its own
         * session.
         */
        database_model->add_collection(session, opt.key_count);
        collection &coll = database_model->get_collection(i);
        scoped_cursor cursor = session.open_scoped_cursor(coll.name);
        transaction txn;
        for (int64_t j = 0; j < opt.key_count; j++) {
            txn.begin(session);
            wt_timestamp_t commit_ts = ++ts;
            testutil_check(session->timestamp_transaction(session.get(),
              ("commit_timestamp=" + timestamp_manager::decimal_to_hex(commit_ts)).c_str()));
            testutil_assert(crud::insert(cursor, txn, generate_key(j), generate_value()));
            testutil_assert(txn.commit(session));
            if (j % 1000 == 0)
                /* Advance the stable and oldest timestamps. */
                update_global_timestamps();

            /* Log progress every 20% or at significant milestones. */
            if (j != 0 && opt.key_count > 5 && (j % (opt.key_count / 5) == 0 || j % 50000 == 0))
                logger::log_msg(LOG_INFO,
                  "Populate: Collection " + std::to_string(i + 1) + " - loaded " +
                    std::to_string(j) + "/" + std::to_string(opt.key_count) + " keys (" +
                    std::to_string((j * 100) / opt.key_count) + "%)");
        }
        update_global_timestamps();
        session->checkpoint(session.get(), nullptr);
        logger::log_msg(LOG_INFO,
          "Populate: Collection " + std::to_string(i + 1) + " complete with " +
            std::to_string(opt.key_count) + " keys");
    }
    logger::log_msg(LOG_INFO,
      "Populate: Complete - " + std::to_string(opt.collection_count) + " collections with " +
        std::to_string(opt.key_count) + " keys each.");
}

/*
 * Walk a cursor for a specified number of records, if less than the total number it will bias
 * towards lower collection numbers.
 */
static void
cache_warming(int64_t records)
{
    logger::log_msg(LOG_INFO,
      "Cache warming: Starting to load " + std::to_string(records) + " records into cache.");
    scoped_session session = connection_manager::instance().create_session();
    int64_t record_count = 0;
    int64_t log_interval = records / 10; /* Log every 10%. */
    if (log_interval == 0)
        log_interval = 1;
    for (int64_t i = 0; i < opt.collection_count; ++i) {
        collection &coll = database_model->get_collection(i);
        scoped_cursor cursor = session.open_scoped_cursor(coll.name);
        for (int64_t j = 0; j < opt.key_count && record_count < records; j++, record_count++) {
            /*
             * We should always be within the record count of the collection, therefore this should
             * never return an error.
             */
            testutil_check(cursor->next(cursor.get()));
            if (record_count > 0 && record_count % log_interval == 0)
                logger::log_msg(LOG_INFO,
                  "Cache warming: Loaded " + std::to_string(record_count) + "/" +
                    std::to_string(records) + " records (" +
                    std::to_string((record_count * 100) / records) + "%)");
        }
    }
    logger::log_msg(
      LOG_INFO, "Cache warming: Complete - loaded " + std::to_string(record_count) + " records.");
}

static void
append(collection &coll, scoped_session &session, scoped_cursor &cursor, uint64_t &ingested_data)
{
    uint64_t start_key_count = coll.get_key_count();
    for (int j = 0; j < 10; j++) {
        transaction txn;
        txn.begin(session);
        testutil_check(session->timestamp_transaction(
          session.get(), ("commit_timestamp=" + timestamp_manager::decimal_to_hex(++ts)).c_str()));
        testutil_assert(
          crud::insert(cursor, txn, generate_key(j + start_key_count), generate_value()));
        testutil_assert(txn.commit(session));
        ingested_data += opt.key_size + opt.value_size;
    }
    coll.increase_key_count(10);
}

static void
update(collection &coll, scoped_session &session, scoped_cursor &cursor, uint64_t &ingested_data)
{
    uint64_t key_count = coll.get_key_count();
    testutil_assert(key_count != 0);
    for (int j = 0; j < 10; j++) {
        transaction txn;
        uint64_t key = random_generator::instance().generate_integer(0UL, key_count - 1);
        std::string k = generate_key(key);

        txn.begin(session);
        testutil_check(session->timestamp_transaction(
          session.get(), ("commit_timestamp=" + timestamp_manager::decimal_to_hex(++ts)).c_str()));

        /* Read the current value (optional, but as per prompt). */
        cursor->set_key(cursor.get(), k.c_str());
        /* All keys must exist. */
        testutil_check(cursor->search(cursor.get()));
        /* Overwrite with a new value. */
        testutil_assert(crud::update(cursor, txn, k, generate_value()));
        testutil_assert(txn.commit(session));
        ingested_data += opt.key_size + opt.value_size;
    }
}

static void
crud_worker(workload_type type)
{
    scoped_session session = connection_manager::instance().create_session();
    struct collection_cursor {
        collection_cursor(collection &coll, scoped_cursor &&cursor)
            : coll(coll), cursor(std::move(cursor))
        {
        }
        scoped_cursor cursor;
        collection &coll;
    };
    std::map<int, collection_cursor> cursor_map;
    uint64_t ingested_data = 0;
    uint64_t last_logged_mb = 0;
    uint64_t target_bytes = opt.ingest_size_mb * 1000ULL * 1000ULL;
    logger::log_msg(LOG_INFO,
      "Ingest: Starting " + std::string(type == workload_type::append ? "append" : "update") +
        " workload, target " + std::to_string(opt.ingest_size_mb) + "MB");
    while (ingested_data < target_bytes) {
        /* Generate a random int between 0 and collection count. */
        int collection_num =
          random_generator::instance().generate_integer(0, opt.collection_count - 1);
        if (cursor_map.find(collection_num) == cursor_map.end()) {
            collection &coll = database_model->get_collection(collection_num);
            /*
             * Construct the mapped value in-place. Using operator[] and then assigning
             * requires the mapped_type to be assignable which is not true here because
             * `collection_cursor` holds a reference member (and a move-only cursor). That
             * deletes the implicit assignment operator. Emplace avoids assignment by
             * constructing the value directly in the map.
             */
            cursor_map.emplace(
              collection_num, collection_cursor(coll, session.open_scoped_cursor(coll.name)));
        }
        /*
         * Access the stored scoped_cursor member from the mapped value. Use `at` to avoid
         * accidental default-construction if the key were missing.
         */
        collection_cursor &cc = cursor_map.at(collection_num);
        scoped_cursor &cursor = cc.cursor;
        /* Workload logic. */
        if (type == workload_type::append)
            append(cc.coll, session, cursor, ingested_data);
        else if (type == workload_type::update)
            update(cc.coll, session, cursor, ingested_data);
        /* Log every 100MB or 10% progress, whichever is less frequent. */
        uint64_t current_mb = ingested_data / 1000 / 1000;
        uint64_t log_interval = std::max(100ULL, opt.ingest_size_mb / 10ULL);
        if (current_mb >= last_logged_mb + log_interval) {
            logger::log_msg(LOG_INFO,
              "Ingest: " + std::string(type == workload_type::append ? "Appended " : "Updated ") +
                std::to_string(current_mb) + "MB / " + std::to_string(opt.ingest_size_mb) + "MB (" +
                std::to_string((current_mb * 100) / opt.ingest_size_mb) + "%)");
            last_logged_mb = current_mb;
        }
    }
    logger::log_msg(LOG_INFO,
      "Ingest: Complete - " +
        std::string(type == workload_type::append ? "appended " : "updated ") +
        std::to_string(ingested_data / 1000 / 1000) + "MB");
}

static void
crud_operations()
{
    /* The user could request to not ingest any data. */
    if (opt.ingest_size_mb == 0) {
        logger::log_msg(LOG_INFO, "Skipping workload phase, ingest data size is 0.");
        return;
    }
    logger::log_msg(LOG_INFO,
      "Starting " + std::string(opt.type == workload_type::append ? "append" : "update") +
        " workload phase.");
    crud_worker(opt.type);
    logger::log_msg(LOG_INFO, "Workload phase complete.");
}

/*
 * wt_disagg_pick_up_latest_checkpoint --
 *     Pick up the latest WiredTiger checkpoint.
 */
static uint64_t
wt_disagg_pick_up_latest_checkpoint()
{
    WT_CONNECTION *conn = connection_manager::instance().get_connection();
    scoped_session session = connection_manager::instance().create_session();
    WT_PAGE_LOG *page_log;
    testutil_check(conn->get_page_log(conn, "palite", &page_log));

    WT_ITEM metadata{};
    uint64_t timestamp;
    testutil_check(page_log->pl_get_complete_checkpoint_ext(
      page_log, session.get(), nullptr, nullptr, &timestamp, &metadata));

    page_log->terminate(page_log, NULL); /* dereference */
    page_log = NULL;

    char *checkpoint_meta = strndup((const char *)metadata.data, metadata.size);
    free(metadata.mem);

    std::ostringstream config;
    config << "disaggregated=(checkpoint_meta=\"" << checkpoint_meta << "\")";
    free(checkpoint_meta);

    std::string config_str = config.str();
    testutil_check(conn->reconfigure(conn, config_str.c_str()));
    return timestamp;
}

int
main(int argc, char *argv[])
{
    /* Set the program name for error messages. */
    const std::string progname = testutil_set_progname(argv);

    /* Set the tracing level for the logger component. */
    logger::trace_level = LOG_INFO;
    logger::log_msg(LOG_INFO, "Starting " + progname);

    /* Parse options. */
    std::string err;
    if (!parse_options(argc, argv, opt, err)) {
        std::cerr << "error: " << err << "\n";
        std::cerr << "usage: " << argv[0] << " [options]\n"
                  << "  -c N        collection_count (int > 0)\n"
                  << "  -g N        cache_size_gb (int > 0)\n"
                  << "  -h PATH     home_path\n"
                  << "  -i N        ingest_size_mb (int > 0)\n"
                  << "  -k N        key_count (int > 0)\n"
                  << "  -s N        key_size (int > 0)\n"
                  << "  -v N        value_size (int > 0)\n"
                  << "  -V N        verbosity level; 1 turns on WT_VERB_DISAGG:1, 2\n"
                  << "              enables palite module logging at verbosity 1, etc.\n"
                  << "  -w N        warm_cache_pct: warm cache as % of initial data\n"
                  << "  -C          create a copy of the loaded data\n"
                  << "  -L          use data in WT_TEST.back instead of loading\n"
                  << "  -S SHAPE    workload_shape ('append' or 'updates')\n";
        return 1;
    }

    logger::log_msg(LOG_INFO, "Running with configuration:");
    logger::log_msg(LOG_INFO, "  collection_count = " + std::to_string(opt.collection_count));
    logger::log_msg(LOG_INFO, "  cache_size_gb = " + std::to_string(opt.cache_size_gb));
    logger::log_msg(LOG_INFO, "  key_count = " + std::to_string(opt.key_count));
    logger::log_msg(LOG_INFO, "  key_size = " + std::to_string(opt.key_size));
    logger::log_msg(LOG_INFO, "  value_size = " + std::to_string(opt.value_size));
    logger::log_msg(LOG_INFO, "  ingest_size_mb = " + std::to_string(opt.ingest_size_mb));
    logger::log_msg(LOG_INFO, "  verbose_level = " + std::to_string(opt.verbose_level));
    logger::log_msg(LOG_INFO,
      "  workload_shape = " +
        std::string(opt.type == workload_type::append ? "append" : "updates"));
    logger::log_msg(LOG_INFO, "  home_path = " + opt.home_path);
    logger::log_msg(LOG_INFO, "  warm_cache_pct = " + std::to_string(opt.warm_cache_pct) + "%");
    logger::log_msg(LOG_INFO, "  load_copy = " + std::string(opt.load_copy ? "true" : "false"));
    logger::log_msg(LOG_INFO, "  load_skip = " + std::string(opt.load_skip ? "true" : "false"));

    logger::log_msg(LOG_INFO,
      "Data size is: " +
        std::to_string(
          ((1ULL * opt.collection_count * opt.key_count) * (opt.key_size + opt.value_size)) / 1000 /
          1000) +
        "MB");

    /* Clean up any artifacts from prior runs. */
    testutil_remove(opt.home_path.c_str());

    /* Initialize. */
    initialize();

    std::string shared_open_config = CONNECTION_CREATE +
      ",cache_size=" + std::to_string(opt.cache_size_gb) + "GB,precise_checkpoint=true";
    std::string extension_config = ",extensions=[../../ext/page_log/palite/libwiredtiger_palite.so";
    std::string shared_disagg_config = ",disaggregated=(page_log=palite";

    /* Populate the database. */
    if (opt.load_skip) {
        logger::log_msg(LOG_INFO, "Skipping populate - using existing database.");
        logger::log_msg(
          LOG_INFO, "Copying \"" + opt.home_path + ".back\" to \"" + opt.home_path + "\"");
        testutil_copy_fast(std::string(opt.home_path + ".back").c_str(), opt.home_path.c_str());
        database_model->add_existing_collections(opt.collection_count, opt.key_count);
        logger::log_msg(LOG_INFO, "Database copy complete.");
    } else {
        connection_manager::instance().create(
          shared_open_config + extension_config + "]" + shared_disagg_config + ",role=\"leader\",)",
          opt.home_path);
        /*
         * We take a checkpoint as the very last stop of populate, this means we don't need to
         * abandon any work. Abandoning a checkpoint is very slow and makes the perf tests results
         * relatively meaningless.*
         */
        populate();
    }
    /* Restart WiredTiger in follower mode. */
    logger::log_msg(LOG_INFO, "##########################################################");
    logger::log_msg(LOG_INFO, "################ Restarting WiredTiger. ##################");
    logger::log_msg(LOG_INFO, "##########################################################");

    connection_manager::instance().close();
    if (opt.load_copy) {
        logger::log_msg(LOG_INFO, "Creating backup copy of the database.");
        logger::log_msg(
          LOG_INFO, "Copying \"" + opt.home_path + "\" to \"" + opt.home_path + ".back\"");
        logger::log_msg(LOG_INFO, "This will delete any existing backup directory.");
        testutil_remove(std::string(opt.home_path + ".back").c_str());
        testutil_copy_fast(opt.home_path.c_str(), std::string(opt.home_path + ".back").c_str());
        logger::log_msg(LOG_INFO, "Backup copy complete.");
    }

    logger::log_msg(LOG_INFO, "##########################################################");
    logger::log_msg(LOG_INFO, "############ Starting WiredTiger as follower. ############");
    logger::log_msg(LOG_INFO, "##########################################################");

    std::string other_config =
      ",statistics_log=(json,wait=1,on_close),statistics=(all),file_manager=(close_idle_time=600,"
      "close_handle_minimum=2000)";
    connection_manager::instance().reopen(shared_open_config + shared_disagg_config +
        ",role=\"follower\",)" + other_config + extension_config +
        (opt.verbose_level > 1 ?
            "=(config=\"(verbose=" + std::to_string(opt.verbose_level - 1) + ")\")" :
            "") +
        "]," +
        (opt.verbose_level >= 1 ? "verbose=(disaggregated_storage:" +
              std::to_string(
                opt.verbose_level % 2 == 0 ? opt.verbose_level - 1 : opt.verbose_level) +
              ")" :
                                  ""),
      opt.home_path);
    WT_CONNECTION *conn = connection_manager::instance().get_connection();

    /* If we loaded an existing database, query the stable timestamp. */
    if (opt.load_skip) {
        logger::log_msg(LOG_INFO, "Querying stable timestamp from existing database.");
        char timestamp[256];
        conn->query_timestamp(conn, timestamp, "get=stable");
        uint64_t stable_timestamp = timestamp_manager::hex_to_decimal(std::string(timestamp));
        logger::log_msg(LOG_INFO, "Stable timestamp = " + std::to_string(stable_timestamp));
        ts = stable_timestamp + 1;
    }

    /* Pickup the latest checkpoint after starting in follower mode. */
    logger::log_msg(LOG_INFO, "Picking up latest checkpoint in follower mode.");
    wt_timestamp_t timestamp = wt_disagg_pick_up_latest_checkpoint();
    logger::log_msg(LOG_INFO, "Checkpoint picked up, timestamp = " + std::to_string(timestamp));

    /* Optionally scan created tables to warm the WT cache. */
    if (opt.warm_cache_pct > 0)
        cache_warming(opt.collection_count * opt.key_count * opt.warm_cache_pct / 100);

    crud_operations();

    logger::log_msg(LOG_INFO, "Re-configuring connection to leader mode.");
    conn->reconfigure(conn, "disaggregated=(role=\"leader\")");
    std::string stable_config = "stable_timestamp=" + timestamp_manager::decimal_to_hex(timestamp);
    conn->set_timestamp(conn, stable_config.c_str());
    logger::log_msg(LOG_INFO, "Reconfiguration complete, stable timestamp set.");
    /* Sleep for 10 seconds, hopefully this will help with FTDC files. */
    logger::log_msg(LOG_INFO, "Sleeping 10 seconds to allow FTDC files to flush.");
    std::this_thread::sleep_for(std::chrono::seconds(10));

    /* Retrieve any useful statistics. */
    logger::log_msg(LOG_INFO, "Retrieving statistics.");
    scoped_session stat_session = connection_manager::instance().create_session();
    scoped_cursor stat_cursor = stat_session.open_scoped_cursor("statistics:");
    int64_t step_up_time = metrics_monitor::get_stat(stat_cursor, WT_STAT_CONN_DISAGG_STEP_UP_TIME);
    /* Add the statistics to metrics_writer and output to JSON file. */
    metrics_writer::instance().add_stat("disagg_step_up_time", step_up_time);
    metrics_writer::instance().output_perf_file(progname);
    logger::log_msg(LOG_INFO, "Statistics written to " + progname + ".json");

    /* Cleanup. */
    delete database_model;
    logger::log_msg(LOG_INFO, "Test completed successfully.");
    return (0);
}
