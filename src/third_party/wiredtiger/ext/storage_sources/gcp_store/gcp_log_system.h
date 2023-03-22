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
#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#include <google/cloud/log.h>

#include <atomic>

// Mapping the desired WiredTiger extension verbosity level to a rough equivalent GCP
// SDK verbosity level.
static const std::map<int32_t, google::cloud::Severity> verbosity_mapping = {
  {WT_VERBOSE_ERROR, google::cloud::Severity::GCP_LS_ERROR},
  {WT_VERBOSE_WARNING, google::cloud::Severity::GCP_LS_WARNING},
  {WT_VERBOSE_INFO, google::cloud::Severity::GCP_LS_INFO},
  {WT_VERBOSE_DEBUG_1, google::cloud::Severity::GCP_LS_DEBUG},
  {WT_VERBOSE_DEBUG_2, google::cloud::Severity::GCP_LS_DEBUG},
  {WT_VERBOSE_DEBUG_3, google::cloud::Severity::GCP_LS_DEBUG},
  {WT_VERBOSE_DEBUG_4, google::cloud::Severity::GCP_LS_DEBUG},
  {WT_VERBOSE_DEBUG_5, google::cloud::Severity::GCP_LS_TRACE}};

/*
 * Provides the GCP Store with a logger implementation that redirects the generated logs to
 * WiredTiger's logging streams. This class implements GCP's LogBackend class, an interface for
 * logging implementations. Functions are derived from the interface to incorporate the logging with
 * WiredTiger's logging system.
 *
 * GCP's LogSink is used to initialize the log system to the SDK.
 */
class gcp_log_system : public google::cloud::LogBackend {
public:
    explicit gcp_log_system(WT_EXTENSION_API *wt_api, int32_t wt_verbosity_level);

    void Process(const google::cloud::LogRecord &log_record) override;

    void
    ProcessWithOwnership(google::cloud::LogRecord log_record) override
    {
        Process(log_record);
    };
    // Inherited from LogBackend and is not used.
    void
    Flush() override final
    {
    }

    // Sends error messages to WiredTiger's error level log stream.
    void
    log_error_message(const std::string &message) const
    {
        log_verbose_message(WT_VERBOSE_ERROR, message);
    }

    // Sends error messages to WiredTiger's debug level log stream.
    void
    log_debug_message(const std::string &message) const
    {
        log_verbose_message(WT_VERBOSE_DEBUG_1, message);
    }

    // Sets the WiredTiger Extension's verbosity level and matches the GCP log levels
    // to this.
    void set_wt_verbosity_level(int32_t wtVerbosityLevel);

private:
    void log_verbose_message(int32_t verbosity_level, const std::string &message) const;
    google::cloud::Severity _gcp_log_level;
    WT_EXTENSION_API *_wt_api;
    int32_t _wt_verbosity_level;
};
