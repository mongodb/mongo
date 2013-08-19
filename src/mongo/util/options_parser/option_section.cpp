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

namespace mongo {
namespace optionenvironment {

    // Registration interface

    Status OptionSection::addSection(const OptionSection& subSection) {
        if (!subSection._positionalOptions.empty()) {
            return Status(ErrorCodes::InternalError,
                          "Attempted to add subsection with positional options");
        }
        _subSections.push_back(subSection);
        return Status::OK();
    }

    Status OptionSection::addOption(const OptionDescription& option) {
        // Verify that neither the single name nor the dotted name for this option conflicts with
        // the names for any options we have already registered
        std::vector<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            if (option._dottedName == oditerator->_dottedName) {
                StringBuilder sb;
                sb << "Attempted to register option with duplicate dottedName: "
                   << option._dottedName;
                return Status(ErrorCodes::InternalError, sb.str());
            }
            if (option._singleName == oditerator->_singleName) {
                StringBuilder sb;
                sb << "Attempted to register option with duplicate singleName: "
                   << option._singleName;
                return Status(ErrorCodes::InternalError, sb.str());
            }
        }
        _options.push_back(option);
        return Status::OK();
    }

    Status OptionSection::addPositionalOption(const PositionalOptionDescription& positionalOption) {
        // Verify that the name for this positional option does not conflict with the name for any
        // positional option we have already registered
        std::vector<PositionalOptionDescription>::const_iterator poditerator;
        for (poditerator = _positionalOptions.begin();
             poditerator != _positionalOptions.end(); poditerator++) {
            if (positionalOption._name == poditerator->_name) {
                StringBuilder sb;
                sb << "Attempted to register duplicate positional option: "
                   << positionalOption._name;
                return Status(ErrorCodes::InternalError, sb.str());
            }
        }
        Status ret = addOption(OptionDescription(positionalOption._name,
                                                 positionalOption._name,
                                                 positionalOption._type,
                                                 "hidden description",
                                                 false/*hidden*/));
        if (!ret.isOK()) {
            return ret;
        }
        _positionalOptions.push_back(positionalOption);
        return Status::OK();
    }

    // Stuff for dealing with Boost

    namespace {
        /** Helper function to convert the values of our OptionType enum into the classes that
         *  boost::program_option uses to pass around this information
         */
        po::value_semantic* typeToBoostType(OptionType type) {
            switch (type) {
                case StringVector:     return po::value< std::vector<std::string> >();
                case Bool:             return po::value<bool>();
                case Double:           return po::value<double>();
                case Int:              return po::value<int>();
                case Long:             return po::value<long>();
                case String:           return po::value<std::string>();
                case UnsignedLongLong: return po::value<unsigned long long>();
                case Unsigned:         return po::value<unsigned>();
                case Switch:           return po::bool_switch();
                default:               return NULL; /* XXX: should not get here */
            }
        }
    } // namespace

    Status OptionSection::getBoostOptions(po::options_description* boostOptions,
                                          bool visibleOnly) const {

        std::vector<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            if (!visibleOnly || (oditerator->_isVisible)) {
                boostOptions->add_options()(oditerator->_singleName.c_str(),
                        typeToBoostType(oditerator->_type),
                        oditerator->_description.c_str());
            }
        }

        std::vector<OptionSection>::const_iterator ositerator;
        for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
            po::options_description subGroup = ositerator->_name.empty()
                                               ? po::options_description()
                                               : po::options_description(ositerator->_name.c_str());
            ositerator->getBoostOptions(&subGroup, visibleOnly);
            boostOptions->add(subGroup);
        }

        return Status::OK();
    }

    Status OptionSection::getBoostPositionalOptions(
                            po::positional_options_description* boostPositionalOptions) const {

        std::vector<PositionalOptionDescription>::const_iterator poditerator;
        for (poditerator = _positionalOptions.begin();
             poditerator != _positionalOptions.end(); poditerator++) {
            boostPositionalOptions->add(poditerator->_name.c_str(), poditerator->_count);
        }

        return Status::OK();
    }

    // Get options for iterating
    // TODO: should I make this an iterator?

    Status OptionSection::getAllOptions(std::vector<OptionDescription>* options) const {

        std::vector<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            options->push_back(*oditerator);
        }

        std::vector<OptionSection>::const_iterator ositerator;
        for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
            ositerator->getAllOptions(options);
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
        std::vector<PositionalOptionDescription>::const_iterator poditerator;
        for (poditerator = _positionalOptions.begin();
             poditerator != _positionalOptions.end(); poditerator++) {
            std::cout << " _name: " << poditerator->_name
                    << " _type: " << poditerator->_type
                    << " _count: " << poditerator->_count << std::endl;
        }

        std::vector<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            std::cout << " _dottedName: " << oditerator->_dottedName
                    << " _singleName: " << oditerator->_singleName
                    << " _type: " << oditerator->_type
                    << " _description: " << oditerator->_description
                    << " _isVisible: " << oditerator->_isVisible << std::endl;
        }

        std::vector<OptionSection>::const_iterator ositerator;
        for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
            std::cout << "Section Name: " << ositerator->_name << std::endl;
            ositerator->dump();
        }
    }

} // namespace optionenvironment
} // namespace mongo
