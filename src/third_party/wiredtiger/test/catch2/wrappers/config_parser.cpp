/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#include <string>
#include "config_parser.h"

config_parser::config_parser(const std::map<std::string, std::string> &map)
    : _config_map(map), _cfg{nullptr, nullptr, nullptr}
{
}

const std::string &
config_parser::get_config_value(const std::string &config) const
{
    return _config_map.at(config);
}

void
config_parser::insert_config(const std::string &config, const std::string &value)
{
    _config_map[config] = value;
}

bool
config_parser::erase_config(const std::string &config)
{
    auto it = _config_map.find(config);
    if (it == _config_map.end())
        return false;
    _config_map.erase(it);
    return true;
}

const char **
config_parser::get_config_array()
{
    std::string config_string;
    for (const auto &config : _config_map)
        config_string += config.first + "=" + config.second + ",";
    _config_string = config_string;
    _cfg[0] = _config_string.data();
    return const_cast<const char **>(_cfg);
}
