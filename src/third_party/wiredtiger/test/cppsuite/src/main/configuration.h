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
#include <vector>

extern "C" {
#include "wiredtiger.h"
}

namespace test_harness {
inline std::vector<std::string>
split_string(const std::string &str, const char delim)
{
    std::vector<std::string> splits;
    std::string current_str;
    for (const auto c : str) {
        if (c == delim) {
            if (!current_str.empty()) {
                splits.push_back(current_str);
                current_str.clear();
            }
        } else
            current_str.push_back(c);
    }
    if (!current_str.empty())
        splits.push_back(std::move(current_str));
    return (splits);
}

class configuration {
public:
    configuration(const std::string &test_config_name, const std::string &config);
    explicit configuration(const WT_CONFIG_ITEM &nested);

    ~configuration();

    /*
     * Wrapper functions for retrieving basic configuration values. Ideally tests can avoid using
     * the config item struct provided by wiredtiger.
     *
     * When getting a configuration value that may not exist for that configuration string or
     * component, the optional forms of the functions can be used. In this case a default value must
     * be passed and it will be set to that value.
     */
    bool get_bool(const std::string &key);
    bool get_optional_bool(const std::string &key, const bool def);
    int64_t get_int(const std::string &key);
    int64_t get_optional_int(const std::string &key, const int64_t def);
    configuration *get_subconfig(const std::string &key);
    configuration *get_optional_subconfig(const std::string &key);
    std::string get_string(const std::string &key);
    std::string get_optional_string(const std::string &key, const std::string &def);
    std::vector<std::string> get_list(const std::string &key);
    std::vector<std::string> get_optional_list(const std::string &key);

    /* Get the sleep time from the configuration in ms. */
    uint64_t get_throttle_ms();

private:
    enum class types { BOOL, INT, LIST, STRING, STRUCT };

    template <typename T>
    T get(const std::string &key, bool optional, types type, T def, T (*func)(WT_CONFIG_ITEM item));

    /*
     * Merge together two configuration strings, the user one and the default one.
     */
    static std::string merge_default_config(
      const std::string &default_config, const std::string &user_config);

    /*
     * Split a config string into keys and values, taking care to not split incorrectly when we have
     * a sub config or array.
     */
    static std::vector<std::pair<std::string, std::string>> split_config(const std::string &config);

    static bool comparator(
      std::pair<std::string, std::string> a, std::pair<std::string, std::string> b);

private:
    std::string _config;
    WT_CONFIG_PARSER *_config_parser = nullptr;
};
} // namespace test_harness

#endif
