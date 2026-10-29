// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mongo {
namespace optionenvironment {

constexpr Seconds kDefaultConfigExpandTimeout{30};

class Environment;
class OptionSection;
class Value;


/**
 * Flags controlling whether or not __rest and/or __exec directives in a
 * config file should be expanded via HttpClient/shellExec.
 */
struct ConfigExpand {
    bool rest = false;
    bool exec = false;
    Seconds timeout = kDefaultConfigExpandTimeout;
};

/**
 * Reads the contents of `filename` and puts the result in `outContents`.
 */
Status readRawFile(const std::string& filename,
                   std::string* outContents,
                   ConfigExpand configExpand);

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
 *      exit(ExitCode::fail);
 *  }
 *
 *  bool displayHelp;
 *  ret = environment.get(moe::Key("help"), &displayHelp);
 *  if (!ret.isOK()) {
 *      // Help is a switch, so it should always be set
 *      cout << "Should not get here" << std::endl;
 *      exit(ExitCode::fail);
 *  }
 *  if (displayHelp) {
 *      cout << options.helpString() << std::endl;
 *      exit(ExitCode::clean);
 *  }
 *
 *  // Get the value of port from the environment
 *  int port = 27017;
 *  ret = environment.get(moe::Key("port"), &port);
 *  if (ret.isOK()) {
 *      // We have overridden port here, otherwise it stays as the default.
 *  }
 */
class [[MONGO_MOD_OPEN]] OptionsParser {
public:
    /** Indicates if unknown config options are allowed or not.
     *
     *  true - unknown config options will generate an error during parsing.
     *  false - unknow config options will be ignored during parsing.
     */
    static std::function<bool()> useStrict;

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
    Status run(const OptionSection& options,
               const std::vector<std::string>& argv,
               Environment* env);

    /** Handles parsing of a YAML or INI formatted string. The
     *  OptionSection be a description of the allowed options.  This function populates the
     *  given Environment with the results but does not call validate on the Environment.
     */
    Status runConfigFile(const OptionSection& options, const std::string& config, Environment* env);

private:
    /** Handles parsing of the command line and adds the results to the given Environment */
    Status parseCommandLine(const OptionSection&,
                            const std::vector<std::string>& argv,
                            Environment*);

    /** Handles parsing of an INI config std::string and adds the results to the given Environment
     * */
    Status parseINIConfigFile(const OptionSection&, const std::string& config, Environment*);

    /** Handles parsing of either YAML or INI config and adds the results to the given Environment
     */
    Status parseConfigFile(const OptionSection&,
                           const std::string& argv,
                           Environment*,
                           const ConfigExpand& configExpand);

    /** Gets defaults from the OptionSection and adds them to the given Environment */
    Status addDefaultValues(const OptionSection&, Environment*);

    /** Reads the given config file into the output string.  This function is virtual for
     *  testing purposes only. */
    virtual Status readConfigFile(const std::string& filename,
                                  std::string*,
                                  ConfigExpand configExpand);
};

}  // namespace optionenvironment
}  // namespace mongo
