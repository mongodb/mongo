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
#include "mongo/util/options_parser/value.h"

namespace mongo {
namespace optionenvironment {

    // Registration interface

    namespace {
        /**
         * Utility function check that the type of our Value matches our OptionType
         */
        Status checkValueType(OptionType type, Value value) {
            switch (type) {
                case StringVector:
                    {
                        std::vector<std::string>  valueType;
                        return value.get(&valueType);
                    }
                case Bool:
                    {
                        bool valueType;
                        return value.get(&valueType);
                    }
                case Double:
                    {
                        double valueType;
                        return value.get(&valueType);
                    }
                case Int:
                    {
                        int valueType;
                        return value.get(&valueType);
                    }
                case Long:
                    {
                        long valueType;
                        return value.get(&valueType);
                    }
                case String:
                    {
                        std::string valueType;
                        return value.get(&valueType);
                    }
                case UnsignedLongLong:
                    {
                        unsigned long long valueType;
                        return value.get(&valueType);
                    }
                case Unsigned:
                    {
                        unsigned valueType;
                        return value.get(&valueType);
                    }
                case Switch:
                    {
                        bool valueType;
                        return value.get(&valueType);
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

    // TODO: Make sure the section we are adding does not have duplicate options
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

        // Make sure the type of our default value matches our declared type
        if (!option._default.isEmpty()) {
            Status ret = checkValueType(option._type, option._default);
            if (!ret.isOK()) {
                StringBuilder sb;
                sb << "Could not register option \"" << option._dottedName << "\": "
                << "mismatch between declared type and type of default value: "
                << ret.toString();
                return Status(ErrorCodes::TypeMismatch, sb.str());
            }
        }

        // Make sure that if we are registering a composing option it has the type of StringVector
        if (option._isComposing) {
            if (option._type != StringVector) {
                StringBuilder sb;
                sb << "Could not register option \"" << option._dottedName << "\": "
                   << "only options registered as StringVector can be composing";
                return Status(ErrorCodes::TypeMismatch, sb.str());
            }
        }

        // Disallow registering a default for a composing option since the interaction between the
        // two is unclear (for example, should we override or compose the default)
        if (option._isComposing && !option._default.isEmpty()) {
            StringBuilder sb;
            sb << "Could not register option \"" << option._dottedName << "\": "
                << "Cannot register a default value for a composing option";
            return Status(ErrorCodes::InternalError, sb.str());
        }

        // Disallow registering an implicit value for a composing option since the interaction
        // between the two is unclear
        if (option._isComposing && !option._implicit.isEmpty()) {
            StringBuilder sb;
            sb << "Could not register option \"" << option._dottedName << "\": "
                << "Cannot register an implicit value for a composing option";
            return Status(ErrorCodes::InternalError, sb.str());
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

        // Don't register this positional option if it has already been registered to support
        // positional options that we also want to be visible command line flags
        //
        // TODO: More robust way to do this.  This only works if we register the flag first
        std::vector<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            if (positionalOption._name == oditerator->_dottedName) {
                _positionalOptions.push_back(positionalOption);
                return Status::OK();
            }

            if (positionalOption._name == oditerator->_singleName) {
                _positionalOptions.push_back(positionalOption);
                return Status::OK();
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
                                          bool includeDefaults) const {

        std::vector<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            if (!visibleOnly || (oditerator->_isVisible)) {
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

        std::vector<OptionSection>::const_iterator ositerator;
        for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
            po::options_description subGroup = ositerator->_name.empty()
                                               ? po::options_description()
                                               : po::options_description(ositerator->_name.c_str());
            ositerator->getBoostOptions(&subGroup, visibleOnly, includeDefaults);
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

    Status OptionSection::getDefaults(std::map<Key, Value>* values) const {

        std::vector<OptionDescription>::const_iterator oditerator;
        for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
            if (!oditerator->_default.isEmpty()) {
                (*values)[oditerator->_dottedName] = oditerator->_default;
            }
        }

        std::vector<OptionSection>::const_iterator ositerator;
        for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
            ositerator->getDefaults(values);
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
