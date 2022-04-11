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

#include "../connection_manager.h"
#include "../core/configuration.h"
#include "../util/scoped_types.h"
#include "workload_tracking.h"

namespace test_harness {
workload_tracking::workload_tracking(configuration *_config,
  const std::string &operation_table_config, const std::string &operation_table_name,
  const std::string &schema_table_config, const std::string &schema_table_name,
  const bool use_compression, timestamp_manager &tsm)
    : component("workload_tracking", _config), _operation_table_config(operation_table_config),
      _operation_table_name(operation_table_name), _schema_table_config(schema_table_config),
      _schema_table_name(schema_table_name), _use_compression(use_compression), _tsm(tsm)
{
}

const std::string &
workload_tracking::get_schema_table_name() const
{
    return (_schema_table_name);
}

const std::string &
workload_tracking::get_operation_table_name() const
{
    return (_operation_table_name);
}

void
workload_tracking::load()
{
    component::load();

    if (!_enabled)
        return;

    /* Initiate schema tracking. */
    _session = connection_manager::instance().create_session();
    testutil_check(
      _session->create(_session.get(), _schema_table_name.c_str(), _schema_table_config.c_str()));
    _schema_track_cursor = _session.open_scoped_cursor(_schema_table_name);
    logger::log_msg(LOG_TRACE, "Schema tracking initiated");

    /* Initiate operations tracking. */
    testutil_check(_session->create(
      _session.get(), _operation_table_name.c_str(), _operation_table_config.c_str()));
    logger::log_msg(LOG_TRACE, "Operations tracking created");

    /*
     * Open sweep cursor in a dedicated sweep session. This cursor will be used to clear out
     * obsolete data from the tracking table.
     */
    _sweep_session = connection_manager::instance().create_session();
    _sweep_cursor = _sweep_session.open_scoped_cursor(_operation_table_name);
    logger::log_msg(LOG_TRACE, "Tracking table sweep initialized");
}

void
workload_tracking::do_work()
{
    WT_DECL_RET;
    wt_timestamp_t ts, oldest_ts;
    uint64_t collection_id, sweep_collection_id;
    int op_type;
    const char *key, *value;
    char *sweep_key;
    bool globally_visible_update_found;

    key = sweep_key = nullptr;
    globally_visible_update_found = false;

    /* Take a copy of the oldest so that we sweep with a consistent timestamp. */
    oldest_ts = _tsm.get_oldest_ts();

    /* We need to check if the component is still running to avoid unnecessary iterations. */
    while (_running && (ret = _sweep_cursor->prev(_sweep_cursor.get())) == 0) {
        testutil_check(_sweep_cursor->get_key(_sweep_cursor.get(), &collection_id, &key, &ts));
        testutil_check(_sweep_cursor->get_value(_sweep_cursor.get(), &op_type, &value));
        /*
         * If we're on a new key, reset the check. We want to track whether we have a globally
         * visible update for the current key.
         */
        if (sweep_key == nullptr || sweep_collection_id != collection_id ||
          strcmp(sweep_key, key) != 0) {
            globally_visible_update_found = false;
            if (sweep_key != nullptr)
                free(sweep_key);
            sweep_key = static_cast<char *>(dstrdup(key));
            sweep_collection_id = collection_id;
        }
        if (ts <= oldest_ts) {
            if (globally_visible_update_found) {
                if (logger::trace_level == LOG_TRACE)
                    logger::log_msg(LOG_TRACE,
                      std::string("workload tracking: Obsoleted update, key=") + sweep_key +
                        ", collection_id=" + std::to_string(collection_id) +
                        ", timestamp=" + std::to_string(ts) +
                        ", oldest_timestamp=" + std::to_string(oldest_ts) + ", value=" + value);
                testutil_check(_sweep_cursor->remove(_sweep_cursor.get()));
            } else if (static_cast<tracking_operation>(op_type) == tracking_operation::INSERT) {
                if (logger::trace_level == LOG_TRACE)
                    logger::log_msg(LOG_TRACE,
                      std::string("workload tracking: Found globally visible update, key=") +
                        sweep_key + ", collection_id=" + std::to_string(collection_id) +
                        ", timestamp=" + std::to_string(ts) +
                        ", oldest_timestamp=" + std::to_string(oldest_ts) + ", value=" + value);
                globally_visible_update_found = true;
            }
        }
    }

    free(sweep_key);

    /*
     * If we get here and the test is still running, it means we must have reached the end of the
     * table. We can also get here because the test is no longer running. In this case, the cursor
     * can either be at the end of the table or still on a valid entry since we interrupted the
     * work.
     */
    if (ret != 0 && ret != WT_NOTFOUND)
        testutil_die(LOG_ERROR,
          "Tracking table sweep failed: cursor->next() returned an unexpected error %d.", ret);

    /* If we have a position, give it up. */
    testutil_check(_sweep_cursor->reset(_sweep_cursor.get()));
}

void
workload_tracking::save_schema_operation(
  const tracking_operation &operation, const uint64_t &collection_id, wt_timestamp_t ts)
{
    std::string error_message;

    if (!_enabled)
        return;

    if (operation == tracking_operation::CREATE_COLLECTION ||
      operation == tracking_operation::DELETE_COLLECTION) {
        _schema_track_cursor->set_key(_schema_track_cursor.get(), collection_id, ts);
        _schema_track_cursor->set_value(_schema_track_cursor.get(), static_cast<int>(operation));
        testutil_check(_schema_track_cursor->insert(_schema_track_cursor.get()));
    } else {
        error_message =
          "save_schema_operation: invalid operation " + std::to_string(static_cast<int>(operation));
        testutil_die(EINVAL, error_message.c_str());
    }
}
} // namespace test_harness
