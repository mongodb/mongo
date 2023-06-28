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

#pragma once
#include <wiredtiger.h>
#include <wiredtiger_ext.h>

#include <azure/core/diagnostics/logger.hpp>

#include <map>

const std::map<int32_t, Azure::Core::Diagnostics::Logger::Level> wt_to_azure_verbosity_mapping = {
  {WT_VERBOSE_ERROR, Azure::Core::Diagnostics::Logger::Level::Error},
  {WT_VERBOSE_WARNING, Azure::Core::Diagnostics::Logger::Level::Warning},
  {WT_VERBOSE_INFO, Azure::Core::Diagnostics::Logger::Level::Informational},
  {WT_VERBOSE_DEBUG_1, Azure::Core::Diagnostics::Logger::Level::Verbose},
  {WT_VERBOSE_DEBUG_2, Azure::Core::Diagnostics::Logger::Level::Verbose},
  {WT_VERBOSE_DEBUG_3, Azure::Core::Diagnostics::Logger::Level::Verbose},
  {WT_VERBOSE_DEBUG_4, Azure::Core::Diagnostics::Logger::Level::Verbose},
  {WT_VERBOSE_DEBUG_5, Azure::Core::Diagnostics::Logger::Level::Verbose},
};

/*
 * This class represents the Azure Logging System which is used for all logging output, with
 * configurable logging levels. The Azure errors are mapped to their WiredTiger equivalent and
 * logged through the use of the WT_EXTENSION_API.
 */
class azure_log_system {
public:
    explicit azure_log_system(WT_EXTENSION_API *wt_api, int32_t wt_verbosity_level);
    const Azure::Core::Diagnostics::Logger::Level wt_to_azure_verbosity_level(
      int32_t wt_verbosity_level);
    void set_wt_verbosity_level(int32_t wt_verbosity_level);
    void log_err_msg(const std::string &message) const;
    void log_debug_message(const std::string &message) const;
    void azure_log_listener(Azure::Core::Diagnostics::Logger::Level lvl, std::string msg);

private:
    void log_verbose_message(int32_t verbosity_level, const std::string &message) const;
    WT_EXTENSION_API *_wt_api;
    int32_t _wt_verbosity_level;
    Azure::Core::Diagnostics::Logger::Level _azure_log_level;
};
