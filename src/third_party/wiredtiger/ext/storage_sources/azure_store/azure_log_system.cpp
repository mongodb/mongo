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

#include <cassert>

#include "azure_log_system.h"

// Constructor for azure_log_system that calls to set the WiredTiger verbosity level.
azure_log_system::azure_log_system(WT_EXTENSION_API *wt_api, int32_t wt_verbosity_level)
    : _wt_api(wt_api)
{
    set_wt_verbosity_level(wt_verbosity_level);
}

// Find Azure Logger level given WiredTiger verbosity level returns Warning if not found.
const Azure::Core::Diagnostics::Logger::Level
azure_log_system::wt_to_azure_verbosity_level(int32_t wt_verbosity_level)
{
    auto res = wt_to_azure_verbosity_mapping.find(wt_verbosity_level);
    assert(res != wt_to_azure_verbosity_mapping.end());
    if (res != wt_to_azure_verbosity_mapping.end())
        return res->second;
    else
        return Azure::Core::Diagnostics::Logger::Level::Warning;
}

// Sets the WiredTiger verbosity level by mapping the Azure SDK log level.
void
azure_log_system::set_wt_verbosity_level(int32_t wt_verbosity_level)
{
    _wt_verbosity_level = wt_verbosity_level;
    _azure_log_level = wt_to_azure_verbosity_level(wt_verbosity_level);
    Azure::Core::Diagnostics::Logger::SetLevel(_azure_log_level);
}

void
azure_log_system::log_verbose_message(int32_t verbosity_level, const std::string &message) const
{
    if (verbosity_level <= _wt_verbosity_level) {
        if (verbosity_level < WT_VERBOSE_NOTICE)
            _wt_api->err_printf(_wt_api, NULL, "%s", message.c_str());
        else
            _wt_api->msg_printf(_wt_api, NULL, "%s", message.c_str());
    }
}

void
azure_log_system::log_err_msg(const std::string &message) const
{
    log_verbose_message(WT_VERBOSE_ERROR, message);
}

void
azure_log_system::log_debug_message(const std::string &message) const
{
    log_verbose_message(WT_VERBOSE_DEBUG_1, message);
}

void
azure_log_system::azure_log_listener(Azure::Core::Diagnostics::Logger::Level lvl, std::string msg)
{
    if (lvl <= _azure_log_level)
        _wt_api->err_printf(_wt_api, NULL, "%s", msg.c_str());
    else
        _wt_api->msg_printf(_wt_api, NULL, "%s", msg.c_str());
}
