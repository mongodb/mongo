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
#include "mongo/util/options_parser/option_description.h"

#include <boost/program_options.hpp>
#include <iostream>

#include "mongo/base/status.h"

namespace mongo {
namespace optionenvironment {

    namespace po = boost::program_options;

    /** A container for OptionDescription instances and PositionalOptionDescription instances as
     *  well as other OptionSection instances.  Provides a description of all options that are
     *  supported to be passed in to an OptionsParser.  Has utility functions to support the various
     *  formats needed by the parsing process
     *
     *  Usage:
     *
     *  namespace moe = mongo::optionenvironment;
     *
     *  moe::OptionsParser parser;
     *  moe::Environment environment;
     *  moe::OptionSection options;
     *  moe::OptionSection subSection("Section Name");
     *
     *  // Register our allowed option flags with our OptionSection
     *  options.addOption(moe::OptionDescription("help", "help", moe::Switch, "Display Help"));
     *
     *  // Register our positional options with our OptionSection
     *  options.addPositionalOption(moe::PositionalOptionDescription("command", moe::String));
     *
     *  // Add a subsection
     *  subSection.addOption(moe::OptionDescription("port", "port", moe::Int, "Port"));
     *  options.addSection(subSection);
     *
     *  // Run the parser
     *  Status ret = parser.run(options, argc, argv, envp, &environment);
     *  if (!ret.isOK()) {
     *      cerr << options.helpString() << endl;
     *      exit(EXIT_FAILURE);
     *  }
     */

    class OptionSection {
    public:
        OptionSection(const std::string& name) : _name(name) { }
        OptionSection() { }

        // Construction interface

        /**
         * Add a sub section to this section.  Used mainly to keep track of section headers for when
         * we need generate the help string for the command line
         */
        Status addSection(const OptionSection& subSection);
        /**
         * Add an option to this section
         */
        Status addOption(const OptionDescription& option);
        /**
         * Add a positional option to this section.  Also adds a normal hidden option with the same
         * name as the PositionalOptionDescription because that is the mechanism boost program
         * options uses.  Unfortunately this means that positional options can also be accessed by
         * name in the config files and via command line flags
         */
        Status addPositionalOption(const PositionalOptionDescription& positionalOption);

        // These functions are used by the OptionsParser to make calls into boost::program_options
        Status getBoostOptions(po::options_description* boostOptions,
                               bool visibleOnly = false,
                               bool includeDefaults = false) const;
        Status getBoostPositionalOptions(
                po::positional_options_description* boostPositionalOptions) const;

        // This is needed so that the parser can iterate over all registered options to get the
        // correct names when populating the Environment, as well as check that a parameter that was
        // found has been registered and has the correct type
        Status getAllOptions(std::vector<OptionDescription>* options) const;

        /**
         * Populates the given map with all the default values for any options in this option
         * section and all sub sections.
         */
        Status getDefaults(std::map<Key, Value>* values) const;

        std::string positionalHelpString(const std::string& execName) const;
        std::string helpString() const;

        // Debugging
        void dump() const;

    private:
        std::string _name;
        std::vector<OptionSection> _subSections;
        std::vector<OptionDescription> _options;
        std::vector<PositionalOptionDescription> _positionalOptions;
    };

} // namespace optionenvironment
} // namespace mongo
