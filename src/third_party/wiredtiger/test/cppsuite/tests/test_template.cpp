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

#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/main/test.h"

namespace test_harness {
/* Defines what data is written to the tracking table for use in custom validation. */
class operation_tracker_template : public operation_tracker {

    public:
    operation_tracker_template(
      configuration *config, const bool use_compression, timestamp_manager &tsm)
        : operation_tracker(config, use_compression, tsm)
    {
    }

    void
    set_tracking_cursor(WT_SESSION *session, const tracking_operation &operation,
      const uint64_t &collection_id, const std::string &key, const std::string &value,
      wt_timestamp_t ts, scoped_cursor &op_track_cursor) override final
    {
        /* You can replace this call to define your own tracking table contents. */
        operation_tracker::set_tracking_cursor(
          session, operation, collection_id, key, value, ts, op_track_cursor);
    }
};

/*
 * Class that defines operations that do nothing as an example. This shows how database operations
 * can be overridden and customized.
 */
class test_template : public test {
    public:
    test_template(const test_args &args) : test(args)
    {
        init_operation_tracker(
          new operation_tracker_template(_config->get_subconfig(OPERATION_TRACKER),
            _config->get_bool(COMPRESSION_ENABLED), *_timestamp_manager));
    }

    void
    run() override final
    {
        /* You can remove the call to the base class to fully customize your test. */
        test::run();
    }

    void
    populate(database &, timestamp_manager *, configuration *, operation_tracker *) override final
    {
        logger::log_msg(LOG_WARN, "populate: nothing done");
    }

    void
    checkpoint_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "checkpoint_operation: nothing done");
    }

    void
    custom_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "custom_operation: nothing done");
    }

    void
    insert_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "insert_operation: nothing done");
    }

    void
    read_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "read_operation: nothing done");
    }

    void
    remove_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "remove_operation: nothing done");
    }

    void
    update_operation(thread_worker *) override final
    {
        logger::log_msg(LOG_WARN, "update_operation: nothing done");
    }

    void
    validate(const std::string &, const std::string &, database &) override final
    {
        logger::log_msg(LOG_WARN, "validate: nothing done");
    }
};

} // namespace test_harness
