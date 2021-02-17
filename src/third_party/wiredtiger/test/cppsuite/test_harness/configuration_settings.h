/* Include guard. */
#ifndef CONFIGURATION_SETTINGS_H
#define CONFIGURATION_SETTINGS_H

#include <string>
#include <stdexcept>

#include "wt_internal.h"

namespace test_harness {
class configuration {
    public:
    configuration(const char *test_config_name, const char *config) : _config(config)
    {
        int ret = wiredtiger_config_parser_open(nullptr, config, strlen(config), &_config_parser);
        if (ret != 0)
            throw std::invalid_argument(
              "failed to create configuration parser for provided config");
        if (wiredtiger_test_config_validate(nullptr, nullptr, test_config_name, config) != 0)
            throw std::invalid_argument(
              "failed to validate given config, ensure test config exists");
    }

    configuration(const char *test_config_name, const WT_CONFIG_ITEM &nested)
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

    std::string
    get_config()
    {
        return (_config);
    }

    /*
     * Wrapper functions for retrieving basic configuration values. Ideally the tests can avoid
     * using the config item struct provided by wiredtiger. However if they still wish to use it the
     * get and next functions can be used.
     */
    int
    get_string(const char *key, std::string &value)
    {
        WT_CONFIG_ITEM temp_value;
        WT_RET(_config_parser->get(_config_parser, key, &temp_value));
        if (temp_value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRING ||
          temp_value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID)
            return (-1);
        value = std::string(temp_value.str, temp_value.len);
        return (0);
    }

    int
    get_bool(const char *key, bool &value)
    {
        WT_CONFIG_ITEM temp_value;
        WT_RET(_config_parser->get(_config_parser, key, &temp_value));
        if (temp_value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL)
            return (-1);
        value = temp_value.val != 0;
        return (0);
    }

    int
    get_int(const char *key, int64_t &value)
    {
        WT_CONFIG_ITEM temp_value;
        WT_RET(_config_parser->get(_config_parser, key, &temp_value));
        if (temp_value.type != WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM)
            return (-1);
        value = temp_value.val;
        return (0);
    }

    /*
     * Basic configuration parsing helper functions.
     */
    int
    next(WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value)
    {
        return (_config_parser->next(_config_parser, key, value));
    }

    int
    get(const char *key, WT_CONFIG_ITEM *value)
    {
        return (_config_parser->get(_config_parser, key, value));
    }

    private:
    std::string _config;
    WT_CONFIG_PARSER *_config_parser;
};
} // namespace test_harness

#endif
