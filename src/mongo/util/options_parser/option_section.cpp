/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/util/options_parser/option_section.h"

#include "mongo/bson/util/builder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/options_parser/value.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace mongo {
namespace optionenvironment {

using std::shared_ptr;

// Registration interface

namespace {
Status checkConflicts(const std::set<std::string>& allDottedNames,
                      const std::set<std::string>& allSingleNames,
                      const OptionDescription& option) {
    // Check for duplication in dotted name(s).
    if (allDottedNames.count(option._dottedName)) {
        return {ErrorCodes::InternalError,
                str::stream() << "Attempted to register option with duplicate dottedName: "
                              << option._dottedName};
    }

    for (const auto& name : option._deprecatedDottedNames) {
        // Deprecated name must not already be registered by previous setting,
        // must not match non-deprecated name,
        // and must not match any other single deprecated name.
        if (allDottedNames.count(name) || (option._dottedName == name) ||
            (std::count(option._deprecatedDottedNames.begin(),
                        option._deprecatedDottedNames.end(),
                        name) > 1)) {
            return {ErrorCodes::InternalError,
                    str::stream()
                        << "Attempted to register option with duplicate deprecated dottedName: "
                        << name};
        }
    }

    // Check for duplication in single name(s).
    if (allSingleNames.count(option._singleName)) {
        return {ErrorCodes::InternalError,
                str::stream() << "Attempted to register option with duplicate singleName: "
                              << option._singleName};
    }

    for (const auto& name : option._deprecatedSingleNames) {
        // Deprecated name must not already be registered by previous setting,
        // must not match non-deprecated name,
        // and must not match any other single deprecated name.
        if (allSingleNames.count(name) || (option._singleName == name) ||
            (std::count(option._deprecatedSingleNames.begin(),
                        option._deprecatedSingleNames.end(),
                        name) > 1)) {
            return {ErrorCodes::InternalError,
                    str::stream()
                        << "Attempted to register option with duplicate deprecated singleName: "
                        << name};
        }
    }

    return Status::OK();
}
}  // namespace

Status OptionSection::addSection(const OptionSection& newSection) {
    if (newSection._subSections.size()) {
        return {ErrorCodes::InternalError, "Option subsections may not contain nested subsections"};
    }

    for (const auto& opt : newSection._options) {
        if (opt._positionalStart != -1) {
            return {ErrorCodes::InternalError,
                    str::stream() << "Attempted to add subsection with positional option: "
                                  << opt._dottedName};
        }
        auto status = checkConflicts(_allDottedNames, _allSingleNames, opt);
        if (!status.isOK()) {
            return status;
        }
    }

    _allDottedNames.insert(newSection._allDottedNames.begin(), newSection._allDottedNames.end());
    _allSingleNames.insert(newSection._allSingleNames.begin(), newSection._allSingleNames.end());

    for (auto& oldSection : _subSections) {
        if (newSection._name == oldSection._name) {
            // Matches existing section name, merge options.
            std::copy(newSection._options.cbegin(),
                      newSection._options.cend(),
                      std::back_inserter(oldSection._options));
            return Status::OK();
        }
    }

    // New section name, just adopt it.
    _subSections.push_back(newSection);
    return Status::OK();
}

OptionDescription& OptionSection::addOptionChaining(
    const std::string& dottedName,
    const std::string& singleName,
    const OptionType type,
    const std::string& description,
    const std::vector<std::string>& deprecatedDottedNames,
    const std::vector<std::string>& deprecatedSingleNames,
    OptionParserUsageType) {

    OptionDescription option(
        dottedName, singleName, type, description, deprecatedDottedNames, deprecatedSingleNames);

    // dottedName must be non-empty.
    uassert(ErrorCodes::InternalError,
            "Attempted to register option with empty dottedName",
            !dottedName.empty());

    // Verify deprecated dotted names.
    // No empty deprecated dotted names.
    uassert(ErrorCodes::InternalError,
            "Attempted to register option with empty string for deprecatedDottedName",
            std::count(deprecatedDottedNames.begin(), deprecatedDottedNames.end(), "") == 0);

    // Should not be the same as dottedName.
    uassert(ErrorCodes::InternalError,
            str::stream() << "Attempted to register option with conflict between dottedName and "
                          << "deprecatedDottedName: " << dottedName,
            !std::count(deprecatedDottedNames.begin(), deprecatedDottedNames.end(), dottedName));

    // Verify deprecated single names.
    // No empty deprecated single names.
    uassert(ErrorCodes::InternalError,
            "Attempted to register option with empty string for deprecatedSingleName",
            std::count(deprecatedSingleNames.begin(), deprecatedSingleNames.end(), "") == 0);

    // Should not be the same as singleName.
    uassert(ErrorCodes::InternalError,
            str::stream() << "Attempted to register option with conflict between singleName and "
                          << "deprecatedSingleName: " << singleName,
            !std::count(deprecatedSingleNames.begin(), deprecatedSingleNames.end(), singleName));

    // Should not contain any already registered name.
    uassertStatusOK(checkConflicts(_allDottedNames, _allSingleNames, option));

    _allDottedNames.insert(option._dottedName);
    _allDottedNames.insert(option._deprecatedDottedNames.begin(),
                           option._deprecatedDottedNames.end());
    if (!option._singleName.empty()) {
        _allSingleNames.insert(option._singleName);
    }
    _allSingleNames.insert(option._deprecatedSingleNames.begin(),
                           option._deprecatedSingleNames.end());
    _options.push_back(std::move(option));

    return _options.back();
}

// Stuff for dealing with Boost

namespace {

/*
 * Helper for option types that should be interpreted as a string by boost.  We do this to
 * take the responsibility away from boost for handling type conversions, since sometimes
 * those conversions are inconsistent with our own.  See SERVER-14110 For an example.
 */
template <typename Type>
Status typeToBoostStringType(std::unique_ptr<po::value_semantic>* boostType,
                             const Value defaultValue = Value(),
                             const Value implicitValue = Value()) {
    std::unique_ptr<po::typed_value<std::string>> boostTypeBuilder(po::value<std::string>());

    if (!implicitValue.isEmpty()) {
        Type implicitValueType;
        Status ret = implicitValue.get(&implicitValueType);
        if (!ret.isOK()) {
            StringBuilder sb;
            sb << "Error getting implicit value: " << ret.toString();
            return Status(ErrorCodes::InternalError, sb.str());
        }
        StringBuilder sb;
        sb << implicitValueType;
        boostTypeBuilder->implicit_value(sb.str());
    }

    if (!defaultValue.isEmpty()) {
        Type defaultValueType;
        Status ret = defaultValue.get(&defaultValueType);
        if (!ret.isOK()) {
            StringBuilder sb;
            sb << "Error getting default value: " << ret.toString();
            return Status(ErrorCodes::InternalError, sb.str());
        }
        StringBuilder sb;
        sb << defaultValueType;
        boostTypeBuilder->default_value(sb.str());
    }

    *boostType = std::move(boostTypeBuilder);

    return Status::OK();
}

/** Helper function to convert the values of our OptionType enum into the classes that
 *  boost::program_option uses to pass around this information
 */
Status typeToBoostType(std::unique_ptr<po::value_semantic>* boostType,
                       OptionType type,
                       const Value defaultValue = Value(),
                       const Value implicitValue = Value(),
                       bool getSwitchAsBool = false) {
    switch (type) {
        case StringVector: {
            *boostType = std::unique_ptr<po::value_semantic>(po::value<std::vector<std::string>>());

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
        case StringMap: {
            // Boost doesn't support maps, so we just register a vector parameter and
            // parse it as "key=value" strings
            *boostType = std::unique_ptr<po::value_semantic>(po::value<std::vector<std::string>>());

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
        case Switch: {
            // In boost, switches default to false which makes it impossible to tell if
            // a switch in a config file is not present or was explicitly set to false.
            //
            // Because of this, and because of the fact that we use the same set of
            // options for the legacy key=value config file, we need a way to control
            // whether we are telling boost that an option is a switch type or that an
            // option is a bool type.
            if (!getSwitchAsBool) {
                *boostType = std::unique_ptr<po::value_semantic>(po::bool_switch());
                return Status::OK();
            } else {
                // Switches should be true if they are present with no explicit value.
                *boostType =
                    std::unique_ptr<po::typed_value<bool>>(po::value<bool>()->implicit_value(true));
                return Status::OK();
            }
        }
        case Bool: {
            std::unique_ptr<po::typed_value<bool>> boostTypeBuilder(po::value<bool>());

            if (!implicitValue.isEmpty()) {
                bool implicitValueType;
                Status ret = implicitValue.get(&implicitValueType);
                if (!ret.isOK()) {
                    StringBuilder sb;
                    sb << "Error getting implicit value: " << ret.toString();
                    return Status(ErrorCodes::InternalError, sb.str());
                }
                boostTypeBuilder->implicit_value(implicitValueType);
            }

            if (!defaultValue.isEmpty()) {
                bool defaultValueType;
                Status ret = defaultValue.get(&defaultValueType);
                if (!ret.isOK()) {
                    StringBuilder sb;
                    sb << "Error getting default value: " << ret.toString();
                    return Status(ErrorCodes::InternalError, sb.str());
                }
                boostTypeBuilder->default_value(defaultValueType);
            }

            *boostType = std::move(boostTypeBuilder);

            return Status::OK();
        }
        case String:
            return typeToBoostStringType<std::string>(boostType, defaultValue, implicitValue);
        case Double:
            return typeToBoostStringType<double>(boostType, defaultValue, implicitValue);
        case Int:
            return typeToBoostStringType<int>(boostType, defaultValue, implicitValue);
        case Long:
            return typeToBoostStringType<long>(boostType, defaultValue, implicitValue);
        case UnsignedLongLong:
            return typeToBoostStringType<unsigned long long>(
                boostType, defaultValue, implicitValue);
        case Unsigned:
            return typeToBoostStringType<unsigned>(boostType, defaultValue, implicitValue);
        default: {
            StringBuilder sb;
            sb << "Unrecognized option type: " << type;
            return Status(ErrorCodes::InternalError, sb.str());
        }
    }
}
}  // namespace

Status OptionSection::getBoostOptions(po::options_description* boostOptions,
                                      bool visibleOnly,
                                      bool includeDefaults,
                                      OptionSources sources,
                                      bool getEmptySections) const {
    std::list<OptionDescription>::const_iterator oditerator;
    for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
        // Only include this option if it matches the sources we specified and the option is
        // either visible or we are requesting hidden options
        if ((!visibleOnly || (oditerator->_isVisible)) && (oditerator->_sources & sources)) {
            std::unique_ptr<po::value_semantic> boostType;
            Status ret = typeToBoostType(&boostType,
                                         oditerator->_type,
                                         includeDefaults ? oditerator->_default : Value(),
                                         oditerator->_implicit,
                                         !(sources & SourceCommandLine));
            if (!ret.isOK()) {
                StringBuilder sb;
                sb << "Error getting boost type for option \"" << oditerator->_dottedName
                   << "\": " << ret.toString();
                return Status(ErrorCodes::InternalError, sb.str());
            }

            if (oditerator->_singleName.empty()) {
                StringBuilder sb;
                sb << "Single name is empty for option \"" << oditerator->_dottedName
                   << "\", but trying to use it on the command line "
                   << "or INI config file.  Only options that are exclusive to the YAML config "
                   << "file can have an empty single name";
                return Status(ErrorCodes::InternalError, sb.str());
            }

            boostOptions->add_options()(oditerator->_singleName.c_str(),
                                        boostType.release(),
                                        oditerator->_description.c_str());

            if (!visibleOnly) {
                for (const std::string& depreatedSingleName : oditerator->_deprecatedSingleNames) {
                    std::unique_ptr<po::value_semantic> boostTypeDep;
                    Status retDep =
                        typeToBoostType(&boostTypeDep,
                                        oditerator->_type,
                                        includeDefaults ? oditerator->_default : Value(),
                                        oditerator->_implicit,
                                        !(sources & SourceCommandLine));
                    if (!retDep.isOK()) {
                        return retDep;
                    }
                    boostOptions->add_options()(depreatedSingleName.c_str(),
                                                boostTypeDep.release(),
                                                oditerator->_description.c_str());
                }
            }
        }
    }

    std::list<OptionSection>::const_iterator ositerator;
    for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
        po::options_description subGroup = ositerator->_name.empty()
            ? po::options_description()
            : po::options_description(ositerator->_name.c_str());

        // Do not add empty sections to our option_description unless we specifically requested.
        int numOptions;
        Status ret = ositerator->countOptions(&numOptions, visibleOnly, sources);
        if (!ret.isOK()) {
            return ret;
        }
        if (numOptions == 0 && getEmptySections == false) {
            continue;
        }

        ret = ositerator->getBoostOptions(
            &subGroup, visibleOnly, includeDefaults, sources, getEmptySections);
        if (!ret.isOK()) {
            return ret;
        }
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
                   << "  Expected next option at position: " << nextPosition << ", but \""
                   << poditerator->_dottedName
                   << "\" starts at position: " << poditerator->_positionalStart;
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
                } else {
                    count = (poditerator->_positionalEnd + 1) - poditerator->_positionalStart;
                }

                boostPositionalOptions->add(poditerator->_dottedName.c_str(), count);
                nextPosition += count;
                std::list<OptionDescription>::iterator old_poditerator = poditerator;
                poditerator++;
                positionalOptions.erase(old_poditerator);
            } else {
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
    for (const auto& opt : _options) {
        // We need to check here that we didn't register an option with an empty single name
        // that is allowed on the command line or in an old style config, since we don't have
        // this information available all at once when the option is registered
        if (opt._singleName.empty() && (opt._sources & SourceAllLegacy)) {
            StringBuilder sb;
            return {ErrorCodes::InternalError,
                    str::stream()
                        << "Found option allowed on the command line with an empty singleName: "
                        << opt._dottedName};
        }

        options->push_back(opt);
    }

    for (const auto& section : _subSections) {
        auto status = section.getAllOptions(options);
        if (!status.isOK()) {
            return status;
        }
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
        ositerator->getDefaults(values).transitional_ignore();
    }

    return Status::OK();
}

Status OptionSection::countOptions(int* numOptions, bool visibleOnly, OptionSources sources) const {
    *numOptions = 0;

    std::list<OptionDescription>::const_iterator oditerator;
    for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
        // Only count this option if it matches the sources we specified and the option is
        // either visible or we are requesting hidden options
        if ((!visibleOnly || (oditerator->_isVisible)) && (oditerator->_sources & sources)) {
            (*numOptions)++;
        }
    }

    std::list<OptionSection>::const_iterator ositerator;
    for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
        int numSubOptions = 0;
        ositerator->countOptions(&numSubOptions, visibleOnly, sources).transitional_ignore();
        *numOptions += numSubOptions;
    }

    return Status::OK();
}

size_t OptionSection::countSubSections() const {
    return _subSections.size();
}

Status OptionSection::getConstraints(std::vector<std::shared_ptr<Constraint>>* constraints) const {
    std::list<OptionDescription>::const_iterator oditerator;
    for (oditerator = _options.begin(); oditerator != _options.end(); oditerator++) {
        std::vector<std::shared_ptr<Constraint>>::const_iterator citerator;
        for (citerator = oditerator->_constraints.begin();
             citerator != oditerator->_constraints.end();
             citerator++) {
            constraints->push_back(*citerator);
        }
    }

    std::list<OptionSection>::const_iterator ositerator;
    for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
        ositerator->getConstraints(constraints).transitional_ignore();
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
    po::options_description boostOptions =
        _name.empty() ? po::options_description() : po::options_description(_name.c_str());
    Status ret = getBoostOptions(&boostOptions,
                                 true, /* visibleOnly */
                                 true, /* includeDefaults */
                                 SourceAllLegacy,
                                 false); /* getEmptySections */
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
                  << " _singleName: " << oditerator->_singleName << " _type: " << oditerator->_type
                  << " _description: " << oditerator->_description
                  << " _isVisible: " << oditerator->_isVisible << std::endl;
    }

    std::list<OptionSection>::const_iterator ositerator;
    for (ositerator = _subSections.begin(); ositerator != _subSections.end(); ositerator++) {
        std::cout << "Section Name: " << ositerator->_name << std::endl;
        ositerator->dump();
    }
}

}  // namespace optionenvironment
}  // namespace mongo
