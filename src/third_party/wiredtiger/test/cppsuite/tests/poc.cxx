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

class poc_test : public test_harness::test {
    public:
    poc_test(const std::string &config, int64_t trace_level) : test(config)
    {
        test_harness::_trace_level = trace_level;
    }

    void
    run()
    {
        test::run();
    }
};

const std::string poc_test::test::name = "poc_test";
const std::string poc_test::test::default_config = "config_poc_default.txt";

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

            if (prev_line == line) {
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
print_error(const std::string &str)
{
    std::cerr << "No value given for option " << str << std::endl;
}

int
main(int argc, char *argv[])
{
    std::string cfg, filename;
    int64_t trace_level = 0, error_code = 0;

    /* Parse args
     * -C   : Configuration. Cannot be used with -f.
     * -f   : Filename that contains the configuration. Cannot be used with -C.
     * -t   : Trace level.
     */
    for (int i = 1; (i < argc) && (error_code == 0); ++i) {
        if (std::string(argv[i]) == "-C") {
            if (!filename.empty()) {
                std::cerr << "Option -C cannot be used with -f" << std::endl;
                error_code = -1;
            } else if ((i + 1) < argc)
                cfg = argv[++i];
            else {
                print_error(argv[i]);
                error_code = -1;
            }
        } else if (std::string(argv[i]) == "-f") {
            if (!cfg.empty()) {
                std::cerr << "Option -f cannot be used with -C" << std::endl;
                error_code = -1;
            } else if ((i + 1) < argc)
                filename = argv[++i];
            else {
                print_error(argv[i]);
                error_code = -1;
            }
        } else if (std::string(argv[i]) == "-t") {
            if ((i + 1) < argc)
                trace_level = std::stoi(argv[++i]);
            else {
                print_error(argv[i]);
                error_code = -1;
            }
        }
    }

    if (error_code == 0) {
        /* Check if default configuration should be used. */
        if (cfg.empty() && filename.empty())
            cfg = parse_configuration_from_file(
              "../../../test/cppsuite/configurations/" + poc_test::test::default_config);
        else if (!filename.empty())
            cfg =
              parse_configuration_from_file("../../../test/cppsuite/configurations/" + filename);

        std::cout << "Configuration\t:" << cfg << std::endl;
        std::cout << "Trace level\t:" << trace_level << std::endl;

        poc_test(cfg, trace_level).run();
    }

    return (error_code);
}
