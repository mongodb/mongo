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

#include "test_harness/util/logger.h"
#include "test_harness/test.h"

#include "base_test.cxx"
#include "burst_inserts.cxx"
#include "example_test.cxx"
#include "hs_cleanup.cxx"
#include "search_near_01.cxx"
#include "search_near_02.cxx"
#include "search_near_03.cxx"

/* Declarations to avoid the error raised by -Werror=missing-prototypes. */
const std::string parse_configuration_from_file(const std::string &filename);
void print_help();
int64_t run_test(
  const std::string &test_name, const std::string &config, const std::string &wt_open_config);

const std::string
parse_configuration_from_file(const std::string &filename)
{
    std::string cfg, line, error;
    std::ifstream cFile(filename);

    if (cFile.is_open()) {
        while (getline(cFile, line)) {
            /* Whitespaces are only for readability, they can be removed safely. */
            line.erase(std::remove_if(line.begin(), line.end(), isspace), line.end());
            if (line[0] == '#' || line.empty())
                continue;
            cfg += line;
        }

    } else {
        error = "Couldn't open " + filename + " file for reading.";
        testutil_die(EINVAL, error.c_str());
    }

    return (cfg);
}

void
print_help()
{
    std::cout << "NAME" << std::endl;
    std::cout << "\trun" << std::endl;
    std::cout << std::endl;
    std::cout << "SYNOPSIS" << std::endl;
    std::cout << "\trun [OPTIONS]" << std::endl;
    std::cout << "\trun -C [WIREDTIGER_OPEN_CONFIGURATION]" << std::endl;
    std::cout << "\trun -c [TEST_FRAMEWORK_CONFIGURATION]" << std::endl;
    std::cout << "\trun -f [FILE]" << std::endl;
    std::cout << "\trun -l [TRACE_LEVEL]" << std::endl;
    std::cout << "\trun -t [TEST_NAME]" << std::endl;
    std::cout << std::endl;
    std::cout << "DESCRIPTION" << std::endl;
    std::cout << "\trun  executes the test framework." << std::endl;
    std::cout << "\tIf no test is indicated, all tests are executed." << std::endl;
    std::cout
      << "\tIf no configuration is indicated, the default configuration for each test will be used."
      << std::endl;
    std::cout
      << "\tIf a configuration is indicated, the given configuration will be used either for "
         "all tests or the test indicated."
      << std::endl;
    std::cout << std::endl;
    std::cout << "OPTIONS" << std::endl;
    std::cout << "\t-h Output a usage message and exit." << std::endl;
    std::cout << "\t-C Additional wiredtiger open configuration." << std::endl;
    std::cout << "\t-c Test framework configuration. Cannot be used with -f." << std::endl;
    std::cout << "\t-f File that contains the configuration. Cannot be used with -C." << std::endl;
    std::cout << "\t-l Trace level from 0 to 3. "
                 "1 is the default level, all warnings and errors are logged."
              << std::endl;
    std::cout << "\t-t Test name to be executed." << std::endl;
}

/*
 * Run a specific test.
 * - test_name: specifies which test to run.
 * - config: defines the configuration used for the test.
 */
int64_t
run_test(const std::string &test_name, const std::string &config, const std::string &wt_open_config)
{
    int error_code = 0;

    test_harness::logger::log_msg(LOG_TRACE, "Configuration\t:" + config);

    if (test_name == "base_test")
        base_test(test_harness::test_args{config, test_name, wt_open_config}).run();
    else if (test_name == "example_test")
        example_test(test_harness::test_args{config, test_name, wt_open_config}).run();
    else if (test_name == "hs_cleanup")
        hs_cleanup(test_harness::test_args{config, test_name, wt_open_config}).run();
    else if (test_name == "burst_inserts")
        burst_inserts(test_harness::test_args{config, test_name, wt_open_config}).run();
    else if (test_name == "search_near_01")
        search_near_01(test_harness::test_args{config, test_name, wt_open_config}).run();
    else if (test_name == "search_near_02")
        search_near_02(test_harness::test_args{config, test_name, wt_open_config}).run();
    else if (test_name == "search_near_03")
        search_near_03(test_harness::test_args{config, test_name, wt_open_config}).run();
    else {
        test_harness::logger::log_msg(LOG_ERROR, "Test not found: " + test_name);
        error_code = -1;
    }

    if (error_code == 0)
        test_harness::logger::log_msg(LOG_INFO, "Test " + test_name + " done.");

    return (error_code);
}

static std::string
get_default_config_path(const std::string &test_name)
{
    return ("configs/" + test_name + "_default.txt");
}

int
main(int argc, char *argv[])
{
    std::string cfg, config_filename, current_cfg, current_test_name, test_name, wt_open_config;
    int64_t error_code = 0;
    const std::vector<std::string> all_tests = {"base_test", "burst_inserts", "example_test",
      "hs_cleanup", "search_near_01", "search_near_02", "search_near_03"};

    /* Set the program name for error messages. */
    (void)testutil_set_progname(argv);

    /* Parse args
     * -C   : Additional wiredtiger_open configuration.
     * -c   : Test framework configuration. Cannot be used with -f. If no specific test is specified
     * to be run, the same configuration will be used for all existing tests.
     * -f   : Filename that contains the configuration. Cannot be used with -C. If no specific test
     * is specified to be run, the same configuration will be used for all existing tests.
     * -l   : Trace level.
     * -t   : Test to run. All tests are run if not specified.
     */
    for (size_t i = 1; (i < argc) && (error_code == 0); ++i) {
        if (std::string(argv[i]) == "-h") {
            print_help();
            return 0;
        } else if (std::string(argv[i]) == "-C") {
            if ((i + 1) < argc) {
                wt_open_config = argv[++i];
                /* Add a comma to the front if the user didn't supply one. */
                if (wt_open_config[0] != ',')
                    wt_open_config.insert(0, 1, ',');
            } else
                error_code = -1;
        } else if (std::string(argv[i]) == "-c") {
            if (!config_filename.empty()) {
                test_harness::logger::log_msg(LOG_ERROR, "Option -C cannot be used with -f");
                error_code = -1;
            } else if ((i + 1) < argc)
                cfg = argv[++i];
            else
                error_code = -1;
        } else if (std::string(argv[i]) == "-f") {
            if (!cfg.empty()) {
                test_harness::logger::log_msg(LOG_ERROR, "Option -f cannot be used with -C");
                error_code = -1;
            } else if ((i + 1) < argc)
                config_filename = argv[++i];
            else
                error_code = -1;
        } else if (std::string(argv[i]) == "-t") {
            if ((i + 1) < argc)
                test_name = argv[++i];
            else
                error_code = -1;
        } else if (std::string(argv[i]) == "-l") {
            if ((i + 1) < argc)
                test_harness::logger::trace_level = std::stoi(argv[++i]);
            else
                error_code = -1;
        } else
            error_code = -1;
    }

    if (error_code == 0) {
        test_harness::logger::log_msg(
          LOG_INFO, "Trace level: " + std::to_string(test_harness::logger::trace_level));
        if (test_name.empty()) {
            /* Run all tests. */
            test_harness::logger::log_msg(LOG_INFO, "Running all tests.");
            for (auto const &it : all_tests) {
                current_test_name = it;
                /* Configuration parsing. */
                if (!config_filename.empty())
                    current_cfg = parse_configuration_from_file(config_filename);
                else if (cfg.empty())
                    current_cfg =
                      parse_configuration_from_file(get_default_config_path(current_test_name));
                else
                    current_cfg = cfg;

                error_code = run_test(current_test_name, current_cfg, wt_open_config);
                if (error_code != 0)
                    break;
            }
        } else {
            current_test_name = test_name;
            /* Check the test exists. */
            if (std::find(all_tests.begin(), all_tests.end(), current_test_name) ==
              all_tests.end()) {
                test_harness::logger::log_msg(
                  LOG_ERROR, "The test " + current_test_name + " was not found.");
                error_code = -1;
            } else {
                /* Configuration parsing. */
                if (!config_filename.empty())
                    cfg = parse_configuration_from_file(config_filename);
                else if (cfg.empty())
                    cfg = parse_configuration_from_file(get_default_config_path(current_test_name));
                error_code = run_test(current_test_name, cfg, wt_open_config);
            }
        }

        if (error_code != 0)
            test_harness::logger::log_msg(LOG_ERROR, "Test " + current_test_name + " failed.");
    } else
        test_harness::logger::log_msg(LOG_ERROR,
          "Invalid command line arguments supplied. Try "
          "'./run -h' for help.");

    return (error_code);
}
