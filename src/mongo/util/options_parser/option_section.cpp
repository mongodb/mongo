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

#include "mongo/util/options_parser/option_section.h"

#include <sstream>

#include "mongo/bson/util/builder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/options_parser/value.h"

namespace mongo {
namespace optionenvironment {

    // Registration interface

    // TODO: Make sure the section we are adding does not have duplicate options
    Status OptionSection::addSection(const OptionSection& subSection) {
        std::list<OptionDescription>::const_iterator oditerator;
        for (oditerator = subSection._options.begin();
             oditerator != subSection._options.end(); oditerator++) {
            if (oditerator->_positionalStart != -1) {
                StringBuilder sb;
                sb << "Attempted to add subsection with positional option: "
                   << oditerator->_dottedName;
                return Status(ErrorCodes::InternalError, sb.str());
            }
        }
        _subSections.push_back(subSection);
        return Status::OK();
    }

    OptionDescription& OptionSection::addOptionChaining(const std::string& dottedName,
                                                        const std::string& singleName,
                                                        const OptionType type,
                                                        const std::string& description) {
        OptionDescription option(dottedName, singleName, type, description);

        // Verify that neither the single name nor the dotted name for this option conflicts with
        // the names for any options we have already registered
        std::list<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            if (option._dottedName == oditerator->_dottedName) {
                StringBuilder sb;
                sb << "Attempted to register option with duplicate dottedName: "
                   << option._dottedName;
                throw DBException(sb.str(), ErrorCodes::InternalError);
            }
            if (option._singleName == oditerator->_singleName) {
                StringBuilder sb;
                sb << "Attempted to register option with duplicate singleName: "
                   << option._singleName;
                throw DBException(sb.str(), ErrorCodes::InternalError);
            }
        }

        _options.push_back(option);

        return _options.back();
    }

    // Stuff for dealing with Boost

    namespace {
        /** Helper function to convert the values of our OptionType enum into the classes that
         *  boost::program_option uses to pass around this information
         */
        Status typeToBoostType(std::auto_ptr<po::value_semantic>* boostType,
                OptionType type,
                const Value defaultValue = Value(),
                const Value implicitValue = Value()) {
            switch (type) {
                case StringVector:
                    {
                        *boostType = std::auto_ptr<po::value_semantic>(
                                                    po::value< std::vector<std::string> >());

                        if (!implicitValue.isEmpty()) {
                            StringBuilder sb;
                            sb << "Implicit value not supported for string vector";
                            return Status(ErrorCodes::InternalError, sb.str());
                        }

                        if (!defaultValue.isEmpty()) {
                            StringBuilder sb;
                            sb << "Default value not supported for string vector";
                            return Status(ErrorCodes::InternalError, sb.str());
                        }

                        return Status::OK();
                    }
                case StringMap:
                    {
                        // Boost doesn't support maps, so we just register a vector parameter and
                        // parse it as "key=value" strings
                        *boostType = std::auto_ptr<po::value_semantic>(
                                                    po::value< std::vector<std::string> >());

                        if (!implicitValue.isEmpty()) {
                            StringBuilder sb;
                            sb << "Implicit value not supported for string map";
                            return Status(ErrorCodes::InternalError, sb.str());
                        }

                        if (!defaultValue.isEmpty()) {
                            StringBuilder sb;
                            sb << "Default value not supported for string map";
                            return Status(ErrorCodes::InternalError, sb.str());
                        }

                        return Status::OK();
                    }
                case Bool:
                    {
                        std::auto_ptr<po::typed_value<bool> > boostTypeBuilder(po::value<bool>());

                        if (!implicitValue.isEmpty()) {
                            bool implicitValueType;
                            Status ret = implicitValue.get(&implicitValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting implicit value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->implicit_value(implicitValueType);
                        }

                        if (!defaultValue.isEmpty()) {
                            bool defaultValueType;
                            Status ret = defaultValue.get(&defaultValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting default value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->default_value(defaultValueType);
                        }

                        *boostType = boostTypeBuilder;

                        return Status::OK();
                    }
                case Double:
                    {
                        std::auto_ptr<po::typed_value<double> >
                                        boostTypeBuilder(po::value<double>());

                        if (!implicitValue.isEmpty()) {
                            double implicitValueType;
                            Status ret = implicitValue.get(&implicitValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting implicit value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->implicit_value(implicitValueType);
                        }

                        if (!defaultValue.isEmpty()) {
                            double defaultValueType;
                            Status ret = defaultValue.get(&defaultValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting default value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->default_value(defaultValueType);
                        }

                        *boostType = boostTypeBuilder;

                        return Status::OK();
                    }
                case Int:
                    {
                        std::auto_ptr<po::typed_value<int> > boostTypeBuilder(po::value<int>());

                        if (!implicitValue.isEmpty()) {
                            int implicitValueType;
                            Status ret = implicitValue.get(&implicitValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting implicit value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->implicit_value(implicitValueType);
                        }

                        if (!defaultValue.isEmpty()) {
                            int defaultValueType;
                            Status ret = defaultValue.get(&defaultValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting default value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->default_value(defaultValueType);
                        }

                        *boostType = boostTypeBuilder;

                        return Status::OK();
                    }
                case Long:
                    {
                        std::auto_ptr<po::typed_value<long> > boostTypeBuilder(po::value<long>());

                        if (!implicitValue.isEmpty()) {
                            long implicitValueType;
                            Status ret = implicitValue.get(&implicitValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting implicit value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->implicit_value(implicitValueType);
                        }

                        if (!defaultValue.isEmpty()) {
                            long defaultValueType;
                            Status ret = defaultValue.get(&defaultValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting default value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->default_value(defaultValueType);
                        }

                        *boostType = boostTypeBuilder;

                        return Status::OK();
                    }
                case String:
                    {
                        std::auto_ptr<po::typed_value<std::string> >
                                        boostTypeBuilder(po::value<std::string>());

                        if (!implicitValue.isEmpty()) {
                            std::string implicitValueType;
                            Status ret = implicitValue.get(&implicitValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting implicit value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->implicit_value(implicitValueType);
                        }

                        if (!defaultValue.isEmpty()) {
                            std::string defaultValueType;
                            Status ret = defaultValue.get(&defaultValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting default value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->default_value(defaultValueType);
                        }

                        *boostType = boostTypeBuilder;

                        return Status::OK();
                    }
                case UnsignedLongLong:
                    {
                        std::auto_ptr<po::typed_value<unsigned long long> >
                                        boostTypeBuilder(po::value<unsigned long long>());

                        if (!implicitValue.isEmpty()) {
                            unsigned long long implicitValueType;
                            Status ret = implicitValue.get(&implicitValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting implicit value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->implicit_value(implicitValueType);
                        }

                        if (!defaultValue.isEmpty()) {
                            unsigned long long defaultValueType;
                            Status ret = defaultValue.get(&defaultValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting default value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->default_value(defaultValueType);
                        }

                        *boostType = boostTypeBuilder;

                        return Status::OK();
                    }
                case Unsigned:
                    {
                        std::auto_ptr<po::typed_value<unsigned> >
                                        boostTypeBuilder(po::value<unsigned>());

                        if (!implicitValue.isEmpty()) {
                            unsigned implicitValueType;
                            Status ret = implicitValue.get(&implicitValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting implicit value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->implicit_value(implicitValueType);
                        }

                        if (!defaultValue.isEmpty()) {
                            unsigned defaultValueType;
                            Status ret = defaultValue.get(&defaultValueType);
                            if(!ret.isOK()) {
                                StringBuilder sb;
                                sb << "Error getting default value: " << ret.toString();
                                return Status(ErrorCodes::InternalError, sb.str());
                            }
                            boostTypeBuilder->default_value(defaultValueType);
                        }

                        *boostType = boostTypeBuilder;

                        return Status::OK();
                    }
                case Switch:
                    {
                        *boostType = std::auto_ptr<po::value_semantic>(po::bool_switch());
                        return Status::OK();
                    }
                default:
                    {
                        StringBuilder sb;
                        sb << "Unrecognized option type: " << type;
                        return Status(ErrorCodes::InternalError, sb.str());
                    }
            }
        }
    } // namespace

    Status OptionSection::getBoostOptions(po::options_description* boostOptions,
                                          bool visibleOnly,
                                          bool includeDefaults,
                                          OptionSources sources) const {

        std::list<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            // Only include this option if it matches the sources we specified and the option is
            // either visible or we are requesting hidden options
            if ((!visibleOnly || (oditerator->_isVisible)) &&
                (oditerator->_sources & sources)) {
                std::auto_ptr<po::value_semantic> boostType;
                Status ret = typeToBoostType(&boostType,
                                             oditerator->_type,
                                             includeDefaults ? oditerator->_default : Value(),
                                             oditerator->_implicit);
                if (!ret.isOK()) {
                    StringBuilder sb;
                    sb << "Error getting boost type for option \""
                       << oditerator->_dottedName << "\": " << ret.toString();
                    return Status(ErrorCodes::InternalError, sb.str());
                }
                boostOptions->add_options()(oditerator->_singleName.c_str(),
                        boostType.release(),
                        oditerator->_description.c_str());
            }
        }

        std::list<OptionSection>::const_iterator ositerator;
        for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
            po::options_description subGroup = ositerator->_name.empty()
                                               ? po::options_description()
                                               : po::options_description(ositerator->_name.c_str());
            ositerator->getBoostOptions(&subGroup, visibleOnly, includeDefaults);
            boostOptions->add(subGroup);
        }

        return Status::OK();
    }

    /*
     * The way we specify positional options in our interface differs from the way boost does it, so
     * we have to convert them here.
     *
     * For example, to specify positionals such that you can run "./exec [pos1] [pos2] [pos2]":
     *
     * Our interface:
     *
     * options.addOptionChaining("pos2", "pos2", moe::StringVector, "Pos2")
     *                          .hidden() <- doesn't show up in help
     *                          .sources(moe::SourceCommandLine) <- only allowed on command line
     *                          .positional(2, <- start position
     *                          3); <- end position
     * options.addOptionChaining("pos1", "pos1", moe::String, "Pos1")
     *                          .hidden() <- doesn't show up in help
     *                          .sources(moe::SourceCommandLine) <- only allowed on command line
     *                          .positional(1, <- start position
     *                          1); <- end position
     * // Note that order doesn't matter
     *
     * Boost's interface:
     *
     * boostHiddenOptions->add_options()("pos1", po::value<std::string>(), "Pos1")
     *                                  ("pos2", po::value<std::string>(), "Pos2")
     *
     * boostPositionalOptions->add("pos1", 1); <- count of option (number of times it appears)
     * boostPositionalOptions->add("pos2", 2); <- count of option (number of times it appears)
     * // Note that order does matter
     *
     * Because of this, we have to perform the conversion in this function.  The tasks performed by
     * this function are:
     *
     * 1. Making sure the ranges are valid as a whole (no overlap or holes)
     * 2. Convert to the boost options and add them in the correct order
     */
    Status OptionSection::getBoostPositionalOptions(
                            po::positional_options_description* boostPositionalOptions) const {

        std::list<OptionDescription> positionalOptions;

        std::list<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            // Check if this is a positional option, and extract it if it is
            if (oditerator->_positionalStart != -1) {
                positionalOptions.push_back(*oditerator);
            }
        }

        int nextPosition = 1;
        bool foundAtPosition = false;
        while (!positionalOptions.empty()) {
            foundAtPosition = false;
            std::list<OptionDescription>::iterator poditerator;
            for (poditerator = positionalOptions.begin(); poditerator != positionalOptions.end();) {

                if (poditerator->_positionalStart < nextPosition) {
                    StringBuilder sb;
                    sb << "Found option with overlapping positional range: "
                        << "  Expected next option at position: " << nextPosition
                        << ", but \"" << poditerator->_dottedName << "\" starts at position: "
                        << poditerator->_positionalStart;
                    return Status(ErrorCodes::InternalError, sb.str());
                }

                if (poditerator->_positionalStart == nextPosition) {
                    foundAtPosition = true;

                    int count;
                    if (poditerator->_positionalEnd == -1) {
                        count = -1;
                        if (positionalOptions.size() != 1) {
                            StringBuilder sb;
                            sb << "Found positional option with infinite count, but still have "
                               << "more positional options registered";
                            return Status(ErrorCodes::InternalError, sb.str());
                        }
                    }
                    else {
                        count = (poditerator->_positionalEnd + 1) - poditerator->_positionalStart;
                    }

                    boostPositionalOptions->add(poditerator->_dottedName.c_str(), count);
                    nextPosition += count;
                    std::list<OptionDescription>::iterator old_poditerator = poditerator;
                    poditerator++;
                    positionalOptions.erase(old_poditerator);
                }
                else {
                    poditerator++;
                }
            }
            if (!foundAtPosition) {
                StringBuilder sb;
                sb << "Did not find option at position: " << nextPosition;
                return Status(ErrorCodes::InternalError, sb.str());
            }
        }

        // XXX: Right now only the top level section can have positional options

        return Status::OK();
    }

    // Get options for iterating
    // TODO: should I make this an iterator?

    Status OptionSection::getAllOptions(std::vector<OptionDescription>* options) const {

        std::list<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            options->push_back(*oditerator);
        }

        std::list<OptionSection>::const_iterator ositerator;
        for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
            ositerator->getAllOptions(options);
        }

        return Status::OK();
    }

    Status OptionSection::getDefaults(std::map<Key, Value>* values) const {

        std::list<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            if (!oditerator->_default.isEmpty()) {
                (*values)[oditerator->_dottedName] = oditerator->_default;
            }
        }

        std::list<OptionSection>::const_iterator ositerator;
        for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
            ositerator->getDefaults(values);
        }

        return Status::OK();
    }

    Status OptionSection::getConstraints(
            std::vector<boost::shared_ptr<Constraint > >* constraints) const {

        std::list<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            std::vector<boost::shared_ptr<Constraint> >::const_iterator citerator;
            for (citerator = oditerator->_constraints.begin();
                 citerator != oditerator->_constraints.end(); citerator++) {
                constraints->push_back(*citerator);
            }
        }

        std::list<OptionSection>::const_iterator ositerator;
        for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
            ositerator->getConstraints(constraints);
        }

        return Status::OK();
    }

    std::string OptionSection::positionalHelpString(const std::string& execName) const {

        po::positional_options_description boostPositionalOptions;
        Status ret = getBoostPositionalOptions(&boostPositionalOptions);
        if (!ret.isOK()) {
            StringBuilder sb;
            sb << "Error constructing help string: " << ret.toString();
            return sb.str();
        }

        StringBuilder posHelpStringBuilder;
        posHelpStringBuilder << execName;

        // If we can have unlimited positional options, this returns
        // std::numeric_limits<unsigned>::max().  Check here for that case and record what name to
        // look for.
        unsigned int numPositional = boostPositionalOptions.max_total_count();
        std::string trailingPositionName;
        if (numPositional == std::numeric_limits<unsigned>::max()) {
            trailingPositionName = boostPositionalOptions.name_for_position(numPositional - 1);
        }

        unsigned int position;
        std::string positionName;
        for (position = 0; position < numPositional; position++) {
            positionName = boostPositionalOptions.name_for_position(position);
            if (!trailingPositionName.empty() && trailingPositionName == positionName) {
                // If we have a trailing position, we break when we see it the first time.
                posHelpStringBuilder << " [" << trailingPositionName << " ... ]";
                break;
            }
            posHelpStringBuilder << " [" << positionName << "]";
        }

        return posHelpStringBuilder.str();
    }

    std::string OptionSection::helpString() const {

        po::options_description boostOptions = _name.empty()
                                             ? po::options_description()
                                             : po::options_description(_name.c_str());
        Status ret = getBoostOptions(&boostOptions, true, true);
        if (!ret.isOK()) {
            StringBuilder sb;
            sb << "Error constructing help string: " << ret.toString();
            return sb.str();
        }

        // Can't use a StringBuilder here because boost::program_options only has functions that
        // output to std::ostream
        std::ostringstream os;
        os << boostOptions;
        return os.str();
    }

    /* Debugging */
    void OptionSection::dump() const {

        std::list<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            std::cout << " _dottedName: " << oditerator->_dottedName
                    << " _singleName: " << oditerator->_singleName
                    << " _type: " << oditerator->_type
                    << " _description: " << oditerator->_description
                    << " _isVisible: " << oditerator->_isVisible << std::endl;
        }

        std::list<OptionSection>::const_iterator ositerator;
        for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
            std::cout << "Section Name: " << ositerator->_name << std::endl;
            ositerator->dump();
        }
    }

} // namespace optionenvironment
} // namespace mongo
