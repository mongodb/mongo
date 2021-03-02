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

#ifndef CONFIGURATION_SETTINGS_H
#define CONFIGURATION_SETTINGS_H

#include <stdexcept>
#include <string>

extern "C" {
#include "test_util.h"
}

#include "wt_internal.h"

namespace test_harness {
class configuration {
    public:
    configuration(const std::string &test_config_name, const std::string &config) : _config(config)
    {
        int ret = wiredtiger_test_config_validate(
          nullptr, nullptr, test_config_name.c_str(), config.c_str());
        if (ret != 0)
            throw std::invalid_argument(
              "failed to validate given config, ensure test config exists");
        if (wiredtiger_config_parser_open(
              nullptr, config.c_str(), config.size(), &_config_parser) != 0)
            throw std::invalid_argument(
              "failed to create configuration parser for provided config");
    }

    configuration(const WT_CONFIG_ITEM &nested)
    {
        if (nested.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT)
            throw std::invalid_argument("provided config item isn't a structure");
        int ret = wiredtiger_config_parser_open(nullptr, nested.str, nested.len, &_config_parser);
        if (ret != 0)
            throw std::invalid_argument(
              "failed to create configuration parser for provided sub config");
    }

    ~configuration()
    {
        if (_config_parser != nullptr) {
            _config_parser->close(_config_parser);
            _config_parser = nullptr;
        }
    }

    const std::string &
    get_config() const
    {
        return (_config);
    }

    /*
     * Wrapper functions for retrieving basic configuration values. Ideally the tests can avoid
     * using the config item struct provided by wiredtiger. However if they still wish to use it the
     * get and next functions can be used.
     */
    int
    get_string(const std::string &key, std::string &value) const
    {
        WT_CONFIG_ITEM temp_value;
        testutil_check(_config_parser->get(_config_parser, key.c_str(), &temp_value));
        if (temp_value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRING ||
          temp_value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID)
            return (-1);
        value = std::string(temp_value.str, temp_value.len);
        return (0);
    }

    int
    get_bool(const std::string &key, bool &value) const
    {
        WT_CONFIG_ITEM temp_value;
        testutil_check(_config_parser->get(_config_parser, key.c_str(), &temp_value));
        if (temp_value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL)
            return (-1);
        value = (temp_value.val != 0);
        return (0);
    }

    int
    get_int(const std::string &key, int64_t &value) const
    {
        WT_CONFIG_ITEM temp_value;
        testutil_check(_config_parser->get(_config_parser, key.c_str(), &temp_value));
        if (temp_value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM)
            return (-1);
        value = temp_value.val;
        return (0);
    }

    /*
     * Basic configuration parsing helper functions.
     */
    int
    next(WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value) const
    {
        return (_config_parser->next(_config_parser, key, value));
    }

    int
    get(const std::string &key, WT_CONFIG_ITEM *value) const
    {
        return (_config_parser->get(_config_parser, key.c_str(), value));
    }

    private:
    std::string _config;
    WT_CONFIG_PARSER *_config_parser;
};
} // namespace test_harness

#endif
