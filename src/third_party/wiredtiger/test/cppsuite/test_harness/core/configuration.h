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

#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include <string>

extern "C" {
#include "test_util.h"
}

enum class types { BOOL, INT, STRING, STRUCT };

namespace test_harness {
class configuration {
    public:
    configuration(const std::string &test_config_name, const std::string &config) : _config(config)
    {
        int ret = wiredtiger_test_config_validate(
          nullptr, nullptr, test_config_name.c_str(), config.c_str());
        if (ret != 0)
            testutil_die(EINVAL, "failed to validate given config, ensure test config exists");
        ret =
          wiredtiger_config_parser_open(nullptr, config.c_str(), config.size(), &_config_parser);
        if (ret != 0)
            testutil_die(EINVAL, "failed to create configuration parser for provided config");
    }

    configuration(const WT_CONFIG_ITEM &nested)
    {
        if (nested.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT)
            testutil_die(EINVAL, "provided config item isn't a structure");
        int ret = wiredtiger_config_parser_open(nullptr, nested.str, nested.len, &_config_parser);
        if (ret != 0)
            testutil_die(EINVAL, "failed to create configuration parser for provided sub config");
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
     * Wrapper functions for retrieving basic configuration values. Ideally tests can avoid using
     * the config item struct provided by wiredtiger.
     *
     * When getting a configuration value that may not exist for that configuration string or
     * component, the optional forms of the functions can be used. In this case a default value must
     * be passed and it will be set to that value.
     */
    std::string
    get_string(const std::string &key)
    {
        return get<std::string>(key, false, types::STRING, "", config_item_to_string);
    }

    std::string
    get_optional_string(const std::string &key, const std::string &def)
    {
        return get<std::string>(key, true, types::STRING, def, config_item_to_string);
    }

    bool
    get_bool(const std::string &key)
    {
        return get<bool>(key, false, types::BOOL, false, config_item_to_bool);
    }

    bool
    get_optional_bool(const std::string &key, const bool def)
    {
        return get<bool>(key, true, types::BOOL, def, config_item_to_bool);
    }

    int64_t
    get_int(const std::string &key)
    {
        return get<int64_t>(key, false, types::INT, 0, config_item_to_int);
    }

    int64_t
    get_optional_int(const std::string &key, const int64_t def)
    {
        return get<int64_t>(key, true, types::INT, def, config_item_to_int);
    }

    configuration *
    get_subconfig(const std::string &key)
    {
        return get<configuration *>(key, false, types::STRUCT, nullptr,
          [](WT_CONFIG_ITEM item) { return new configuration(item); });
    }

    private:
    static bool
    config_item_to_bool(const WT_CONFIG_ITEM item)
    {
        return (item.val != 0);
    }

    static int64_t
    config_item_to_int(const WT_CONFIG_ITEM item)
    {
        return (item.val);
    }

    static std::string
    config_item_to_string(const WT_CONFIG_ITEM item)
    {
        return std::string(item.str, item.len);
    }

    template <typename T>
    T
    get(const std::string &key, bool optional, types type, T def, T (*func)(WT_CONFIG_ITEM item))
    {
        WT_DECL_RET;
        WT_CONFIG_ITEM value = {"", 0, 1, WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL};
        const char *error_msg = "Configuration value doesn't match requested type";

        ret = _config_parser->get(_config_parser, key.c_str(), &value);
        if (ret == WT_NOTFOUND && optional)
            return (def);
        else if (ret != 0)
            testutil_die(ret, "Error while finding config");

        if (type == types::STRING &&
          (value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRING &&
            value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID))
            testutil_die(-1, error_msg);
        else if (type == types::BOOL && value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL)
            testutil_die(-1, error_msg);
        else if (type == types::INT && value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM)
            testutil_die(-1, error_msg);
        else if (type == types::STRUCT && value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT)
            testutil_die(-1, error_msg);

        return func(value);
    }

    std::string _config;
    WT_CONFIG_PARSER *_config_parser = nullptr;
};
} // namespace test_harness

#endif
