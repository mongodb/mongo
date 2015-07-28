/* Copyright 2013 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <map>
#include <string>
#include <vector>

#include "mongo/base/status.h"

namespace mongo {
namespace optionenvironment {

class Environment;
class OptionSection;
class Value;

/** Handles parsing of the command line as well as YAML and INI config files.  Takes an
 *  OptionSection instance that describes the allowed options, parses argv (env not yet
 *  supported), and populates an Environment with the results.
 *
 *  Usage:
 *
 *  namespace moe = mongo::optionenvironment;
 *
 *  moe::OptionsParser parser;
 *  moe::Environment environment;
 *  moe::OptionSection options;
 *
 *  // Register our allowed options with our OptionSection
 *  options.addOptionChaining("help", "help", moe::Switch, "Display Help");
 *  options.addOptionChaining("port", "port", moe::Int, "Port");
 *
 *  // Run the parser
 *  Status ret = parser.run(options, argv, env, &environment);
 *  if (!ret.isOK()) {
 *      cerr << options.helpString() << std::endl;
 *      exit(EXIT_FAILURE);
 *  }
 *
 *  bool displayHelp;
 *  ret = environment.get(moe::Key("help"), &displayHelp);
 *  if (!ret.isOK()) {
 *      // Help is a switch, so it should always be set
 *      cout << "Should not get here" << std::endl;
 *      exit(EXIT_FAILURE);
 *  }
 *  if (displayHelp) {
 *      cout << options.helpString() << std::endl;
 *      exit(EXIT_SUCCESS);
 *  }
 *
 *  // Get the value of port from the environment
 *  int port = 27017;
 *  ret = environment.get(moe::Key("port"), &port);
 *  if (ret.isOK()) {
 *      // We have overridden port here, otherwise it stays as the default.
 *  }
 */
class OptionsParser {
public:
    OptionsParser() {}
    virtual ~OptionsParser() {}

    /** Handles parsing of the command line as well as YAML and INI config files.  The
     *  OptionSection be a description of the allowed options.  This function populates the
     *  given Environment with the results of parsing the command line and or config files but
     *  does not call validate on the Environment.
     *
     *  The only special option is the "config" option.  This function will check if the
     *  "config" option was set on the command line and if so attempt to read the given config
     *  file.  For binaries that do not support config files, the "config" option should not be
     *  registered in the OptionSection.
     */
    Status run(const OptionSection&,
               const std::vector<std::string>& argv,
               const std::map<std::string, std::string>& env,
               Environment*);

private:
    /** Handles parsing of the command line and adds the results to the given Environment */
    Status parseCommandLine(const OptionSection&,
                            const std::vector<std::string>& argv,
                            Environment*);

    /** Handles parsing of an INI config std::string and adds the results to the given Environment
     * */
    Status parseINIConfigFile(const OptionSection&, const std::string& config, Environment*);

    /** Gets defaults from the OptionSection and adds them to the given Environment */
    Status addDefaultValues(const OptionSection&, Environment*);

    /** Reads the given config file into the output string.  This function is virtual for
     *  testing purposes only. */
    virtual Status readConfigFile(const std::string& filename, std::string*);
};

}  // namespace optionenvironment
}  // namespace mongo
