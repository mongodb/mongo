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
#include "gcp_log_system.h"

gcp_log_system::gcp_log_system(WT_EXTENSION_API *wt_api, int32_t wt_verbosity_level)
    : _wt_api(wt_api)
{
    set_wt_verbosity_level(wt_verbosity_level);
    google::cloud::LogSink::Instance().set_minimum_severity(_gcp_log_level);
}

void
gcp_log_system::log_verbose_message(int32_t verbosity_level, const std::string &message) const
{
    if (verbosity_level <= _wt_verbosity_level) {
        // Use err_printf for error and warning messages and use msg_printf for notice, info and
        // debug messages.
        if (verbosity_level < WT_VERBOSE_NOTICE)
            _wt_api->err_printf(_wt_api, NULL, "%s", message.c_str());
        else
            _wt_api->msg_printf(_wt_api, NULL, "%s", message.c_str());
    }
}

void
gcp_log_system::Process(const google::cloud::LogRecord &log_record)
{
    _wt_api->err_printf(_wt_api, NULL, "%s", log_record.message.c_str());
}

void
gcp_log_system::set_wt_verbosity_level(int32_t wt_verbosity_level)
{
    _wt_verbosity_level = wt_verbosity_level;
    assert(verbosity_mapping.find(_wt_verbosity_level) != verbosity_mapping.end());
    _gcp_log_level = verbosity_mapping.at(_wt_verbosity_level);
    google::cloud::LogSink::Instance().set_minimum_severity(_gcp_log_level);
}
