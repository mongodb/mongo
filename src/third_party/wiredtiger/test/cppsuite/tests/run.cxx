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

#include <fstream>
#include <iostream>
#include <string>

#include "test_harness/debug_utils.h"
#include "test_harness/test.h"

#include "example_test.cxx"
#include "poc_test.cxx"

std::string
parse_configuration_from_file(const std::string &filename)
{
    std::string cfg, line, prev_line, error;
    std::ifstream cFile(filename);

    if (cFile.is_open()) {
        while (getline(cFile, line)) {

            if (line[0] == '#' || line.empty())
                continue;

            /* Whitespaces are only for readability, they can be removed safely. */
            line.erase(std::remove_if(line.begin(), line.end(), isspace), line.end());

            if (prev_line == line && line != "}") {
                error =
                  "Error when parsing configuration. Two consecutive lines are equal to " + line;
                testutil_die(EINVAL, error.c_str());
                break;
            }

            /* Start of a sub config. */
            if (line == "{")
                cfg += "(";
            /* End of a sub config. */
            else if (line == "}")
                cfg += ")";
            else {
                /* First line. */
                if (cfg.empty())
                    cfg += line;
                /* No comma needed at the start of a subconfig. */
                else if (prev_line == "{")
                    cfg += line;
                else
                    cfg += "," + line;
            }

            prev_line = line;
        }

    } else {
        error = "Couldn't open " + filename + " file for reading.";
        testutil_die(EINVAL, error.c_str());
    }

    return (cfg);
}

void
value_missing_error(const std::string &str)
{
    test_harness::debug_print("Value missing for option " + str, DEBUG_ERROR);
}

/*
 * Run a specific test.
 * config_name is the configuration name. The default configuration is used if it is left empty.
 */
int64_t
run_test(const std::string &test_name, const std::string &config_name = "")
{
    std::string cfg, cfg_path;
    int error_code = 0;

    if (config_name.empty())
        cfg_path = "configs/config_" + test_name + "_default.txt";
    else
        cfg_path = config_name;
    cfg = parse_configuration_from_file(cfg_path);

    test_harness::debug_print("Configuration\t:" + cfg, DEBUG_INFO);

    if (test_name == "poc_test")
        poc_test(cfg, test_name).run();
    else if (test_name == "example_test")
        example_test(cfg, test_name).run();
    else {
        test_harness::debug_print("Test not found: " + test_name, DEBUG_ERROR);
        error_code = -1;
    }

    if (error_code == 0)
        test_harness::debug_print("Test " + test_name + " done.", DEBUG_INFO);

    return (error_code);
}

int
main(int argc, char *argv[])
{
    std::string cfg, config_name, test_name;
    int64_t error_code = 0;
    const std::vector<std::string> all_tests = {"example_test", "poc_test"};

    /* Parse args
     * -C   : Configuration. Cannot be used with -f.
     * -f   : Filename that contains the configuration. Cannot be used with -C.
     * -l   : Trace level.
     * -t   : Test to run. All tests are run if not specified.
     */
    for (int i = 1; (i < argc) && (error_code == 0); ++i) {
        if (std::string(argv[i]) == "-C") {
            if (!config_name.empty()) {
                test_harness::debug_print("Option -C cannot be used with -f", DEBUG_ERROR);
                error_code = -1;
            } else if ((i + 1) < argc)
                cfg = argv[++i];
            else {
                value_missing_error(argv[i]);
                error_code = -1;
            }
        } else if (std::string(argv[i]) == "-f") {
            if (!cfg.empty()) {
                test_harness::debug_print("Option -f cannot be used with -C", DEBUG_ERROR);
                error_code = -1;
            } else if ((i + 1) < argc)
                config_name = argv[++i];
            else {
                value_missing_error(argv[i]);
                error_code = -1;
            }
        } else if (std::string(argv[i]) == "-t") {
            if ((i + 1) < argc)
                test_name = argv[++i];
            else {
                value_missing_error(argv[i]);
                error_code = -1;
            }
        } else if (std::string(argv[i]) == "-l") {
            if ((i + 1) < argc)
                test_harness::_trace_level = std::stoi(argv[++i]);
            else {
                value_missing_error(argv[i]);
                error_code = -1;
            }
        }
    }

    if (error_code == 0) {
        test_harness::debug_print(
          "Trace level\t:" + std::to_string(test_harness::_trace_level), DEBUG_INFO);
        if (test_name.empty()) {
            /* Run all tests. */
            test_harness::debug_print("Running all tests.", DEBUG_INFO);
            for (auto const &it : all_tests) {
                error_code = run_test(it);
                if (error_code != 0) {
                    test_harness::debug_print("Test " + it + " failed.", DEBUG_ERROR);
                    break;
                }
            }
        } else
            error_code = run_test(test_name, config_name);
    }

    return (error_code);
}
