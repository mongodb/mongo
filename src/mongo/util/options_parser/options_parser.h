/* Copyright 2013 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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

    /** Handles parsing of the command line as well as JSON and INI config files.  Takes an
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
     *  options.addOption(moe::OptionDescription("help", "help", moe::Switch, "Display Help"));
     *  options.addOption(moe::OptionDescription("port", "port", moe::Int, "Port"));
     *
     *  // Run the parser
     *  Status ret = parser.run(options, argv, env, &environment);
     *  if (!ret.isOK()) {
     *      cerr << options.helpString() << endl;
     *      exit(EXIT_FAILURE);
     *  }
     *
     *  bool displayHelp;
     *  ret = environment.get(moe::Key("help"), &displayHelp);
     *  if (!ret.isOK()) {
     *      // Help is a switch, so it should always be set
     *      cout << "Should not get here" << endl;
     *      exit(EXIT_FAILURE);
     *  }
     *  if (displayHelp) {
     *      cout << options.helpString() << endl;
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
        OptionsParser() { }
        virtual ~OptionsParser() { }

        /** Handles parsing of the command line as well as JSON and INI config files.  The
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
                                const std::vector<std::string>& argv, Environment*);

        /** Handles parsing of an INI config string and adds the results to the given Environment */
        Status parseINIConfigFile(const OptionSection&, const std::string& config, Environment*);

        /** Handles parsing of a JSON config string and adds the results to the given Environment */
        Status parseJSONConfigFile(const OptionSection&, const std::string& config, Environment*);

        /** Gets defaults from the OptionSection and adds them to the given Environment */
        Status addDefaultValues(const OptionSection&, Environment*);

        /** Detects whether the given string represents a JSON config file or an INI config file */
        bool isJSONConfig(const std::string& config);

        /** Reads the given config file into the output string.  This function is virtual for
         *  testing purposes only. */
        virtual Status readConfigFile(const std::string& filename, std::string*);
    };

} // namespace optionenvironment
} // namespace mongo
