// dbtests.cpp : Runs db unit tests.
//

/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "stdafx.h"

#include <boost/program_options.hpp>

#include "../db/instance.h"
#include "../util/file_allocator.h"

#if !defined(_WIN32)
#include <sys/file.h>
#endif

#include "dbtests.h"

using namespace std;
namespace po = boost::program_options;

namespace mongo {
    extern string dbpath;
}

void show_help_text(const char* name, po::options_description options) {
    cout << "usage: " << name << " [options] [suite]..." << endl
         << options << "suite: run the specified test suite(s) only" << endl;
}

int main( int argc, char** argv ) {
    unsigned long long seed = time( 0 );
    string dbpathSpec;

    po::options_description shell_options("options");
    po::options_description hidden_options("Hidden options");
    po::options_description cmdline_options("Command line options");
    po::positional_options_description positional_options;

    shell_options.add_options()
        ("help,h", "show this usage information")
        ("dbpath", po::value<string>(&dbpathSpec)->default_value("/tmp/unittest/"),
         "db data path for this test run")
        ("debug", "run tests with verbose output")
        ("list,l", "list available test suites")
        ("seed", po::value<unsigned long long>(&seed), "random number seed")
        ;

    hidden_options.add_options()
        ("suites", po::value< vector<string> >(), "test suites to run")
        ;

    positional_options.add("suites", -1);

    cmdline_options.add(shell_options).add(hidden_options);

    po::variables_map params;
    int command_line_style = (((po::command_line_style::unix_style ^
                                po::command_line_style::allow_guessing) |
                               po::command_line_style::allow_long_disguise) ^
                              po::command_line_style::allow_sticky);

    try {
        po::store(po::command_line_parser(argc, argv).options(cmdline_options).
                  positional(positional_options).
                  style(command_line_style).run(), params);
        po::notify(params);
    } catch (po::error &e) {
        cout << "ERROR: " << e.what() << endl << endl;
        show_help_text(argv[0], shell_options);
        return EXIT_BADOPTIONS;
    }

    if (params.count("help")) {
        show_help_text(argv[0], shell_options);
        return EXIT_CLEAN;
    }

    if ( dbpathSpec[ dbpathSpec.length() - 1 ] != '/' )
        dbpathSpec += "/";
    dbpath = dbpathSpec.c_str();

    acquirePathLock();

    srand( seed );
    printGitVersion();
    printSysInfo();
    out() << "random seed: " << seed << endl;

    theFileAllocator().start();

    vector<string> suites;
    if (params.count("suites")) {
        suites = params["suites"].as< vector<string> >();
    }

    int ret = mongo::regression::Suite::run(params.count("debug"),
                                            params.count("list"),
                                            suites);

#if !defined(_WIN32) && !defined(__sunos__)
    flock( lockFile, LOCK_UN );
#endif

    dbexit( (ExitCode)ret ); // so everything shuts down cleanly
    return ret;
}
