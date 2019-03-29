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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/util/options_parser/options_parser.h"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <cerrno>
#include <fstream>
#include <stdio.h>
#include <yaml-cpp/yaml.h>

#include "mongo/base/init.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/http_client.h"
#include "mongo/util/options_parser/constraints.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/shell_exec.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"

namespace mongo {
namespace optionenvironment {

namespace po = boost::program_options;
namespace fs = boost::filesystem;

stdx::function<bool()> OptionsParser::useStrict;

namespace {

bool shouldUseStrict() {
    return true;
}

MONGO_INITIALIZER_GENERAL(OptionsParseUseStrict,
                          MONGO_NO_PREREQUISITES,
                          ("BeginStartupOptionParsing"))
(InitializerContext* context) {
    OptionsParser::useStrict = shouldUseStrict;
    return Status::OK();
}

// The following section contains utility functions that convert between the various objects
// we need to deal with while parsing command line options.
//
// These conversions are different depending on the data source because our current
// implementation uses boost::program_options for the command line and INI files and the
// yaml-cpp YAML parser for YAML config files.  Our destination storage in both cases is an
// Environment which stores Value objects.
//
// 1. YAML Config Files
//     The YAML parser parses a YAML config file into a YAML::Node.  Therefore, we need:
//         a. A function to convert a YAML::Node to a Value (YAMLNodeToValue)
//         b. A function to iterate a YAML::Node, convert the leaf Nodes to Values, and add
//            them to our Environment (addYAMLNodesToEnvironment)
//
// 2. INI Config Files and command line
//     The boost::program_options parsers store their output in a
//     boost::program_options::variables_map.  Therefore, we need:
//         a. A function to convert a boost::any to a Value (boostAnyToValue)
//         b. A function to iterate a variables_map, convert the boost::any elements to
//            Values, and add them to our Environment (addBoostVariablesToEnvironment)

// Attempts to convert a string to a value of the given type.
Status stringToValue(const std::string& stringVal,
                     const OptionType& type,
                     const Key& key,
                     Value* value) {
    Status ret = Status::OK();
    switch (type) {
        double doubleVal;
        int intVal;
        long longVal;
        unsigned long long unsignedLongLongVal;
        unsigned unsignedVal;
        case Switch:
            if (stringVal == "true") {
                *value = Value(true);
                return Status::OK();
            } else if (stringVal == "false") {
                *value = Value(false);
                return Status::OK();
            } else {
                StringBuilder sb;
                sb << "Expected boolean switch but found string: " << stringVal
                   << " for option: " << key;
                return Status(ErrorCodes::BadValue, sb.str());
            }
        case Bool:
            if (stringVal == "true") {
                *value = Value(true);
                return Status::OK();
            } else if (stringVal == "false") {
                *value = Value(false);
                return Status::OK();
            } else {
                StringBuilder sb;
                sb << "Expected boolean but found string: " << stringVal << " for option: " << key;
                return Status(ErrorCodes::BadValue, sb.str());
            }
        case Double:
            ret = parseNumberFromString(stringVal, &doubleVal);
            if (!ret.isOK()) {
                StringBuilder sb;
                sb << "Error parsing option \"" << key << "\" as double in: " << ret.reason();
                return Status(ErrorCodes::BadValue, sb.str());
            }
            *value = Value(doubleVal);
            return Status::OK();
        case Int:
            ret = parseNumberFromString(stringVal, &intVal);
            if (!ret.isOK()) {
                StringBuilder sb;
                sb << "Error parsing option \"" << key << "\" as int: " << ret.reason();
                return Status(ErrorCodes::BadValue, sb.str());
            }
            *value = Value(intVal);
            return Status::OK();
        case Long:
            ret = parseNumberFromString(stringVal, &longVal);
            if (!ret.isOK()) {
                StringBuilder sb;
                sb << "Error parsing option \"" << key << "\" as long: " << ret.reason();
                return Status(ErrorCodes::BadValue, sb.str());
            }
            *value = Value(longVal);
            return Status::OK();
        case String:
            *value = Value(stringVal);
            return Status::OK();
        case UnsignedLongLong:
            ret = parseNumberFromString(stringVal, &unsignedLongLongVal);
            if (!ret.isOK()) {
                StringBuilder sb;
                sb << "Error parsing option \"" << key
                   << "\" as unsigned long long: " << ret.reason();
                return Status(ErrorCodes::BadValue, sb.str());
            }
            *value = Value(unsignedLongLongVal);
            return Status::OK();
        case Unsigned:
            ret = parseNumberFromString(stringVal, &unsignedVal);
            if (!ret.isOK()) {
                StringBuilder sb;
                sb << "Error parsing option \"" << key << "\" as unsigned int: " << ret.reason();
                return Status(ErrorCodes::BadValue, sb.str());
            }
            *value = Value(unsignedVal);
            return Status::OK();
        default: /* XXX: should not get here */
            return Status(ErrorCodes::InternalError, "Unrecognized option type");
    }
}

// Convert a boost::any to a Value.  See comments at the beginning of this section.
Status boostAnyToValue(const boost::any& anyValue,
                       const OptionType& type,
                       const Key& key,
                       Value* value) {

    try {
        if (anyValue.type() == typeid(StringVector_t)) {
            *value = Value(boost::any_cast<StringVector_t>(anyValue));
        } else if (anyValue.type() == typeid(bool)) {
            *value = Value(boost::any_cast<bool>(anyValue));
        } else if (anyValue.type() == typeid(std::string)) {
            return stringToValue(boost::any_cast<std::string>(anyValue), type, key, value);
        }
        // We should not be telling boost about numerical type information.  Instead, for
        // any numerical type we tell boost to read a string value and parse it manually,
        // since boost's parsing is not consistent with ours.  See SERVER-14110.
        else if (anyValue.type() == typeid(double) || anyValue.type() == typeid(int) ||
                 anyValue.type() == typeid(long) || anyValue.type() == typeid(unsigned) ||
                 anyValue.type() == typeid(unsigned long long)) {
            StringBuilder sb;
            sb << "Found int type: " << anyValue.type().name()
               << " in any to Value conversion, which is not supported";
            return Status(ErrorCodes::InternalError, sb.str());
        } else {
            StringBuilder sb;
            sb << "Unrecognized type: " << anyValue.type().name() << " in any to Value conversion";
            return Status(ErrorCodes::InternalError, sb.str());
        }
    } catch (const boost::bad_any_cast& e) {
        StringBuilder sb;
        // We already checked the type, so this is just a sanity check
        sb << "boost::any_cast threw exception: " << e.what();
        return Status(ErrorCodes::InternalError, sb.str());
    }
    return Status::OK();
}

// Returns true if the option for the given key is a StringMap option, and false otherwise
bool OptionIsStringMap(const std::vector<OptionDescription>& options_vector, const Key& key) {
    for (std::vector<OptionDescription>::const_iterator iterator = options_vector.begin();
         iterator != options_vector.end();
         iterator++) {
        if (key == iterator->_dottedName && (iterator->_sources & SourceYAMLConfig)) {
            if (iterator->_type == StringMap) {
                return true;
            }
        }
    }

    return false;
}

Status parseYAMLConfigFile(const std::string&, YAML::Node*);
/* Searches a YAML node for configuration expansion directives such as:
 * __rest: https://example.com/path?query=val
 * __exec: '/usr/bin/getConfig param'
 *
 * and optionally the fields `type` and `trim`.
 *
 * If the field pairing `trim: whitespace` is present,
 * then the process() method will have standard ctype spaces removed from
 * both the front end end of the returned value.
 *
 * If the field pairing `type: yaml` is present (valid only the top-level node)
 * then the process() method will parse any string provided to it as YAML.
 * If the field is not present, or is set to `string`, then process will
 * encapsulate any provided string in a YAML String Node.
 *
 * If no configuration expansion directive is found, the constructor will
 * uassert with ErrorCodes::NoSuchKey.
 */
class ConfigExpandNode {
public:
    ConfigExpandNode(const YAML::Node& node,
                     const std::string& nodePath,
                     const OptionsParser::ConfigExpand& configExpand) {
        invariant(node.IsMap());

        auto nodeName = nodePath;
        auto prefix = nodePath;
        if (nodePath.empty()) {
            nodeName = "Top-level config node";
        } else {
            prefix += '.';
        }

        const auto getStringField = [&node, &prefix](const std::string& fieldName,
                                                     bool allowed) -> boost::optional<std::string> {
            try {
                const auto& strField = node[fieldName];
                if (!strField.IsDefined()) {
                    return {boost::none};
                }
                uassert(ErrorCodes::BadValue,
                        str::stream() << prefix << fieldName << " support has not been enabled",
                        allowed);
                uassert(ErrorCodes::BadValue,
                        str::stream() << prefix << fieldName << " must be a string",
                        strField.IsScalar());
                return strField.Scalar();
            } catch (const YAML::InvalidNode&) {
                // Not this kind of expansion block.
                return {boost::none};
            }
        };

        _expansion = ExpansionType::kRest;
        auto optRestAction = getStringField("__rest", configExpand.rest);
        auto optExecAction = getStringField("__exec", configExpand.exec);
        uassert(ErrorCodes::NoSuchKey,
                "Neither __exec nor __rest specified for config expansion",
                optRestAction || optExecAction);
        uassert(ErrorCodes::BadValue,
                "Must not specify both __rest and __exec in a single config expansion",
                !optRestAction || !optExecAction);

        if (optRestAction) {
            invariant(!optExecAction);
            _expansion = ExpansionType::kRest;
            _action = std::move(*optRestAction);
        } else {
            invariant(optExecAction);
            _expansion = ExpansionType::kExec;
            _action = std::move(*optExecAction);
        }

        // Parse optional fields, keeping track of how many we've handled.
        // If there are additional (unknown) fields beyond that, raise an error.
        size_t numVisitedFields = 1;

        auto optType = getStringField("type", true);
        if (optType) {
            ++numVisitedFields;
            auto typeField = std::move(*optType);
            if (typeField == "string") {
                _type = ContentType::kString;
            } else if (typeField == "yaml") {
                _type = ContentType::kYAML;
            } else {
                uasserted(ErrorCodes::BadValue,
                          str::stream() << prefix << "type must be either 'string' or 'yaml'");
            }
        }

        auto optTrim = getStringField("trim", true);
        if (optTrim) {
            ++numVisitedFields;
            auto trimField = std::move(*optTrim);
            if (trimField == "none") {
                _trim = Trim::kNone;
            } else if (trimField == "whitespace") {
                _trim = Trim::kWhitespace;
            } else {
                uasserted(ErrorCodes::BadValue,
                          str::stream() << prefix << "trim must be either 'whitespace' or 'none'");
            }
        }

        uassert(ErrorCodes::BadValue,
                str::stream() << nodeName << " expansion block must contain only '"
                              << getExpansionName()
                              << "', and optionally 'type' and/or 'trim' fields",
                node.size() == numVisitedFields);

        uassert(ErrorCodes::BadValue,
                "Specifying `type: yaml` is only permitted in top-level expansions",
                nodePath.empty() || (_type == ContentType::kString));
    }

    std::string getExpansionName() const {
        return isRestExpansion() ? "__rest" : "__exec";
    }

    bool isRestExpansion() const {
        return _expansion == ExpansionType::kRest;
    }

    bool isExecExpansion() const {
        return _expansion == ExpansionType::kExec;
    }

    std::string getAction() const {
        return _action;
    }

    /**
     * Optionally trim whitespace from the result of an expansion,
     * and either wrap it as a YAML string node, or process it as a
     * YAML blob.
     */
    YAML::Node process(std::string str) const {
        if (_trim == Trim::kWhitespace) {
            size_t start = 0;
            size_t end = str.size();
            while ((start < end) && std::isspace(str[start])) {
                ++start;
            }
            while ((start < end) && std::isspace(str[end - 1])) {
                --end;
            }
            if ((start > 0) || (end < str.size())) {
                str = str.substr(start, end - start);
            }
        }

        if (_type == ContentType::kString) {
            return YAML::Node(str);
        }

        invariant(_type == ContentType::kYAML);
        YAML::Node newNode;
        const auto status = parseYAMLConfigFile(str, &newNode);
        if (!status.isOK()) {
            uasserted(status.code(),
                      str::stream() << "Failed processing output of " << getExpansionName()
                                    << " block for config file: "
                                    << status.reason());
        }

        return newNode;
    }

private:
    // The type of expansion represented.
    enum class ExpansionType {
        kRest,
        kExec,
    };
    ExpansionType _expansion = ExpansionType::kRest;

    // Whether this expansion represents a single value, or structured YAML.
    enum class ContentType {
        kString,
        kYAML,
    };
    ContentType _type = ContentType::kString;

    // Whether the result of the expansion action should be trimmed for whitespace.
    enum class Trim {
        kNone,
        kWhitespace,
    };
    Trim _trim = Trim::kNone;

    std::string _action;
};

std::string runYAMLRestExpansion(StringData url, Seconds timeout) {

    auto client = HttpClient::create();
    uassert(
        ErrorCodes::OperationFailed, "No HTTP Client available in this build of MongoDB", client);

    // Expect https:// URLs unless we can be sure we're talking to localhost.
    if (!url.startsWith("https://")) {
        uassert(ErrorCodes::BadValue,
                "__rest configuration expansion only supports http/https",
                url.startsWith("http://"));
        const auto start = strlen("http://");
        auto end = url.find('/', start);
        if (end == std::string::npos) {
            end = url.size();
        }
        HostAndPort hp(url.substr(start, end - start));
        client->allowInsecureHTTP(hp.isLocalHost());
    }

    client->setConnectTimeout(timeout);
    client->setTimeout(timeout);

    std::string output;
    auto dataBuilder = client->get(url);
    const auto sz = dataBuilder.size();
    output.resize(sz);
    std::copy_n(dataBuilder.release().get(), sz, &output[0]);

    return output;
}

/* Attempts to parse configuration expansion directives from a config block.
 *
 * If a __rest configuration expansion directive is found,
 * mongo::HttpClient will be invoked to fetch the resource via GET request.
 *
 * If an __exec configuration expansion directive is found,
 * mongo::shellExec() will be invoked to execute the process.
 *
 * See the comment for class ConfigExpandNode for more details.
 */
StatusWith<YAML::Node> runYAMLExpansion(const YAML::Node& node,
                                        const std::string& nodePath,
                                        const OptionsParser::ConfigExpand& configExpand) try {
    invariant(node.IsMap());
    ConfigExpandNode expansion(node, nodePath, configExpand);

    auto nodeName = nodePath;
    auto prefix = nodePath;
    if (nodePath.empty()) {
        nodeName = "Top-level config node";
    } else {
        prefix += '.';
    }

    log() << "Processing " << expansion.getExpansionName() << " config expansion for: " << nodeName;
    const auto action = expansion.getAction();
    LOG(2) << prefix << expansion.getExpansionName() << ": " << action;

    if (expansion.isRestExpansion()) {
        return expansion.process(runYAMLRestExpansion(action, configExpand.timeout));
    }

    invariant(expansion.isExecExpansion());
    // Hard-cap shell expansion at 128MB
    const size_t kShellExpandMaxLenBytes = 128 << 20;
    auto swOutput = shellExec(action, configExpand.timeout, kShellExpandMaxLenBytes);
    if (!swOutput.isOK()) {
        return {ErrorCodes::OperationFailed,
                str::stream() << "Failed expanding __exec section: "
                              << swOutput.getStatus().reason()};
    }
    return expansion.process(std::move(swOutput.getValue()));

} catch (...) {
    return exceptionToStatus();
}

// Convert a YAML::Node to a Value.  See comments at the beginning of this section.
// 'canonicalKey' holds the dotted name that should be used in the result Environment.
// This ensures that both canonical and deprecated dotted names in the configuration
// are mapped to the canonical name.
Status YAMLNodeToValue(const YAML::Node& YAMLNode,
                       const std::vector<OptionDescription>& options_vector,
                       const Key& key,
                       OptionDescription const** option,
                       Value* value,
                       const OptionsParser::ConfigExpand& configExpand) {
    bool isRegistered = false;

    // The logic below should ensure that we don't use this uninitialized, but we need to
    // initialize it here to avoid a compiler warning.  Initializing it to a "Bool" since
    // that's the most restricted type we have and is most likely to result in an early
    // failure if we have a logic error.
    OptionType type = Bool;

    // The config file had a ":" as the first non whitespace character on a line
    if (key.empty()) {
        StringBuilder sb;
        sb << "Found empty key in YAML config file";
        return Status(ErrorCodes::BadValue, sb.str());
    }

    // Get expected type
    for (std::vector<OptionDescription>::const_iterator iterator = options_vector.begin();
         iterator != options_vector.end();
         iterator++) {
        if (!(iterator->_sources & SourceYAMLConfig)) {
            continue;
        }

        bool isDeprecated = std::count(iterator->_deprecatedDottedNames.begin(),
                                       iterator->_deprecatedDottedNames.end(),
                                       key) > 0;
        if (key == iterator->_dottedName || isDeprecated) {
            isRegistered = true;
            type = iterator->_type;
            *option = &*iterator;
            if (isDeprecated) {
                warning() << "Option: " << key << " is deprecated. Please use "
                          << iterator->_dottedName << " instead.";
            }
        }
    }

    if (!isRegistered) {
        if (!OptionsParser::useStrict()) {
            return Status::OK();
        }
        StringBuilder sb;
        sb << "Unrecognized option: " << key;
        return Status(ErrorCodes::BadValue, sb.str());
    }

    // Handle multi keys
    if (type == StringVector) {
        if (!YAMLNode.IsSequence()) {
            StringBuilder sb;
            sb << "Option: " << key
               << " is of type StringVector, but value in YAML config is not a list type";
            return Status(ErrorCodes::BadValue, sb.str());
        }
        StringVector_t stringVector;
        for (YAML::const_iterator it = YAMLNode.begin(); it != YAMLNode.end(); ++it) {
            if (it->IsSequence()) {
                StringBuilder sb;
                sb << "Option: " << key << " has nested lists, which is not allowed";
                return Status(ErrorCodes::BadValue, sb.str());
            }
            stringVector.push_back(it->Scalar());
        }
        *value = Value(stringVector);
        return Status::OK();
    }

    // Handle a sub map as a value
    if (type == StringMap) {
        if (!YAMLNode.IsMap()) {
            StringBuilder sb;
            sb << "Option: " << key
               << " is of type StringMap, but value in YAML config is not a map type";
            return Status(ErrorCodes::BadValue, sb.str());
        }
        StringMap_t stringMap;

        const auto addPair = [&stringMap, &key](std::string elemKey, const YAML::Node& elemVal) {
            if (elemVal.IsSequence() || elemVal.IsMap()) {
                return Status(ErrorCodes::BadValue,
                              str::stream()
                                  << key
                                  << " has a map with non scalar values, which is not allowed");
            }

            if (stringMap.count(elemKey) > 0) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "String Map Option: " << key
                                            << " has duplicate keys in YAML Config: "
                                            << elemKey);
            }

            stringMap[std::move(elemKey)] = elemVal.Scalar();
            return Status::OK();
        };

        for (YAML::const_iterator it = YAMLNode.begin(); it != YAMLNode.end(); ++it) {
            auto elementKey = it->first.Scalar();
            // Because the object returned by dereferencing the `YAMLNode` iterator is an emphemeral
            // proxy value, the objects within it do not get lifetime extension when referred by
            // reference. By making `elementVal` hold a copy of the element, we avoid a bug, found
            // by ASAN, where `elementVal` will be an invalid reference immediately after its
            // creation.
            const auto elementVal = it->second;

            if (elementVal.IsMap()) {
                auto swExpansion = runYAMLExpansion(
                    elementVal, str::stream() << key << "." << elementKey, configExpand);
                if (swExpansion.isOK()) {
                    const auto status = addPair(elementKey, swExpansion.getValue());
                    if (!status.isOK()) {
                        return status;
                    }
                    continue;
                } else if (swExpansion.getStatus().code() != ErrorCodes::NoSuchKey) {
                    return swExpansion.getStatus();
                }  // else not an expansion block.
            }

            const auto status = addPair(std::move(elementKey), elementVal);
            if (!status.isOK()) {
                return status;
            }
        }
        *value = Value(stringMap);
        return Status::OK();
    }

    if (!YAMLNode.IsScalar() && !YAMLNode.IsNull()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Scalar option '" << key << "' must be a single value");
    }

    // Our YAML parser reads everything as a string, so we need to parse it ourselves.
    std::string stringVal = YAMLNode.Scalar();
    return stringToValue(stringVal, type, key, value);
}

Status canonicalizeOption(const OptionDescription& option, Environment* env) {
    if (!option._canonicalize) {
        return Status::OK();
    }

    return option._canonicalize(env);
}

Status checkLongName(const po::variables_map& vm,
                     const std::string& singleName,
                     const OptionDescription& option,
                     Environment* environment,
                     bool* optionAdded) {
    // Trim off the short option from our name so we can look it up correctly in our map
    std::string long_name;
    std::string::size_type commaOffset = singleName.find(',');
    if (commaOffset != std::string::npos) {
        if (commaOffset != singleName.size() - 2) {
            StringBuilder sb;
            sb << "Unexpected comma in option name: \"" << singleName << "\""
               << ": option name must be in the format \"option,o\" or \"option\", "
               << "where \"option\" is the long name and \"o\" is the optional one "
               << "character short alias";
            return Status(ErrorCodes::BadValue, sb.str());
        }
        long_name = singleName.substr(0, commaOffset);
    } else {
        long_name = singleName;
    }

    if (vm.count(long_name)) {
        if (!vm[long_name].defaulted() && singleName != option._singleName) {
            warning() << "Option: " << singleName << " is deprecated. Please use "
                      << option._singleName << " instead.";
        } else if (long_name == "sslMode") {
            warning() << "Option: sslMode is deprecated. Please use tlsMode instead.";
        }

        Value optionValue;
        Status ret = boostAnyToValue(vm[long_name].value(), option._type, long_name, &optionValue);
        if (!ret.isOK()) {
            return ret;
        }

        // If this is really a StringMap, try to split on "key=value" for each element
        // in our StringVector
        if (option._type == StringMap) {
            StringVector_t keyValueVector;
            ret = optionValue.get(&keyValueVector);
            if (!ret.isOK()) {
                return ret;
            }
            StringMap_t mapValue;
            for (StringVector_t::iterator keyValueVectorIt = keyValueVector.begin();
                 keyValueVectorIt != keyValueVector.end();
                 ++keyValueVectorIt) {
                StringData keySD;
                StringData valueSD;
                if (!str::splitOn(*keyValueVectorIt, '=', keySD, valueSD)) {
                    StringBuilder sb;
                    sb << "Illegal option assignment: \"" << *keyValueVectorIt << "\"";
                    return Status(ErrorCodes::BadValue, sb.str());
                }
                std::string key = keySD.toString();
                std::string value = valueSD.toString();
                // Make sure we aren't setting an option to two different values
                if (mapValue.count(key) > 0 && mapValue[key] != value) {
                    StringBuilder sb;
                    sb << "Key Value Option: " << option._dottedName
                       << " has a duplicate key from the same source: " << key;
                    return Status(ErrorCodes::BadValue, sb.str());
                }
                mapValue[key] = value;
            }
            optionValue = Value(mapValue);
        }
        if (!(*optionAdded)) {
            auto ret = environment->set(option._dottedName, optionValue);
            if (!ret.isOK()) {
                return ret;
            }

            ret = canonicalizeOption(option, environment);
            if (!ret.isOK()) {
                return ret;
            }
        } else if (!vm[long_name].defaulted()) {
            StringBuilder sb;
            sb << "Error parsing command line:  Multiple occurrences of option \"" << long_name
               << "\"";
            return Status(ErrorCodes::BadValue, sb.str());
        }
        if (!vm[long_name].defaulted()) {
            *optionAdded = true;
        }
    }

    return Status::OK();
}

// Add all the values in the given variables_map to our environment.  See comments at the
// beginning of this section.
Status addBoostVariablesToEnvironment(const po::variables_map& vm,
                                      const OptionSection& options,
                                      Environment* environment) {
    std::vector<OptionDescription> options_vector;
    Status ret = options.getAllOptions(&options_vector);
    if (!ret.isOK()) {
        return ret;
    }

    for (const OptionDescription& od : options_vector) {

        bool optionAdded = false;
        ret = checkLongName(vm, od._singleName, od, environment, &optionAdded);

        if (!ret.isOK()) {
            return ret;
        }

        for (const std::string& deprecatedSingleName : od._deprecatedSingleNames) {

            ret = checkLongName(vm, deprecatedSingleName, od, environment, &optionAdded);

            if (!ret.isOK()) {
                return ret;
            }
        }
    }
    return Status::OK();
}

// Add all the values in the given YAML Node to our environment.  See comments at the
// beginning of this section.
Status addYAMLNodesToEnvironment(const YAML::Node& root,
                                 const OptionSection& options,
                                 const std::string parentPath,
                                 Environment* environment,
                                 const OptionsParser::ConfigExpand& configExpand) {
    std::vector<OptionDescription> options_vector;
    Status ret = options.getAllOptions(&options_vector);
    if (!ret.isOK()) {
        return ret;
    }

    // Don't return an error on empty config files
    if (root.IsNull()) {
        return Status::OK();
    }

    if (root.IsMap()) {
        auto swExpansion = runYAMLExpansion(root, parentPath, configExpand);
        if (swExpansion.isOK()) {
            // Expanded fine, but disallow recursion.
            return addYAMLNodesToEnvironment(swExpansion.getValue(),
                                             options,
                                             parentPath,
                                             environment,
                                             OptionsParser::ConfigExpand());
        } else if (swExpansion.getStatus().code() != ErrorCodes::NoSuchKey) {
            return swExpansion.getStatus();
        }  // else not an expansion block.
    }

    if (!root.IsMap() && parentPath.empty()) {
        StringBuilder sb;
        sb << "No map found at top level of YAML config";
        return Status(ErrorCodes::BadValue, sb.str());
    }

    for (YAML::const_iterator it = root.begin(); it != root.end(); ++it) {
        std::string fieldName = it->first.Scalar();
        YAML::Node YAMLNode = it->second;

        std::string dottedName;
        if (parentPath.empty()) {
            // We are at the top level, so the full specifier is just the current field name
            dottedName = fieldName;
        } else {
            // If our field name is "value", assume this contains the value for the parent
            if (fieldName == "value") {
                dottedName = parentPath;
            }

            // If this is not a special field name, and we are in a sub object, append our
            // current fieldName to the selector for the sub object we are traversing
            else {
                dottedName = parentPath + '.' + fieldName;
            }
        }

        // Avoid potential double-expand in YAMLNodeToValue by clearing configExpand per-element.
        auto expand = configExpand;

        if (YAMLNode.IsMap()) {
            auto swExpansion = runYAMLExpansion(YAMLNode, dottedName, expand);
            if (swExpansion.isOK()) {
                YAMLNode = std::move(swExpansion.getValue());
                expand = OptionsParser::ConfigExpand();
            } else if (swExpansion.getStatus().code() != ErrorCodes::NoSuchKey) {
                return swExpansion.getStatus();
            }  // else not an expansion block.
        }

        if (YAMLNode.IsMap() && !OptionIsStringMap(options_vector, dottedName)) {
            Status ret =
                addYAMLNodesToEnvironment(YAMLNode, options, dottedName, environment, expand);
            if (!ret.isOK()) {
                return ret;
            }
        } else {
            OptionDescription const* option = nullptr;
            Value optionValue;
            Status ret = YAMLNodeToValue(
                YAMLNode, options_vector, dottedName, &option, &optionValue, expand);
            if (!ret.isOK()) {
                return ret;
            }
            invariant(option);

            Value dummyVal;
            if (environment->get(option->_dottedName, &dummyVal).isOK()) {
                StringBuilder sb;
                sb << "Error parsing YAML config: duplicate key: " << dottedName
                   << "(canonical key: " << option->_dottedName << ")";
                return Status(ErrorCodes::BadValue, sb.str());
            }

            // Only add the value if it is not empty.  YAMLNodeToValue will set the
            // optionValue to an empty Value if we should not set it in the Environment.
            if (!optionValue.isEmpty()) {
                ret = environment->set(option->_dottedName, optionValue);
                if (!ret.isOK()) {
                    return ret;
                }

                ret = canonicalizeOption(*option, environment);
                if (!ret.isOK()) {
                    return ret;
                }
            }
        }
    }

    return Status::OK();
}

/**
* For all options that we registered as composable, combine the values from source and dest
* and set the result in dest.  Note that this only works for options that are registered as
* vectors of strings.
*/
Status addCompositions(const OptionSection& options, const Environment& source, Environment* dest) {
    std::vector<OptionDescription> options_vector;
    Status ret = options.getAllOptions(&options_vector);
    if (!ret.isOK()) {
        return ret;
    }

    for (std::vector<OptionDescription>::const_iterator iterator = options_vector.begin();
         iterator != options_vector.end();
         iterator++) {
        if (!iterator->_isComposing) {
            continue;
        }

        if (iterator->_type == StringVector) {
            StringVector_t sourceValue;
            ret = source.get(iterator->_dottedName, &sourceValue);
            if (!ret.isOK() && ret != ErrorCodes::NoSuchKey) {
                StringBuilder sb;
                sb << "Error getting composable vector value from source: " << ret.toString();
                return Status(ErrorCodes::InternalError, sb.str());
            }
            // Only do something if our source environment has something to add
            else if (ret.isOK()) {
                StringVector_t destValue;
                ret = dest->get(iterator->_dottedName, &destValue);
                if (!ret.isOK() && ret != ErrorCodes::NoSuchKey) {
                    StringBuilder sb;
                    sb << "Error getting composable vector value from dest: " << ret.toString();
                    return Status(ErrorCodes::InternalError, sb.str());
                }

                // Append sourceValue on the end of destValue
                destValue.insert(destValue.end(), sourceValue.begin(), sourceValue.end());

                // Set the resulting value in our output environment
                ret = dest->set(Key(iterator->_dottedName), Value(destValue));
                if (!ret.isOK()) {
                    return ret;
                }

                ret = canonicalizeOption(*iterator, dest);
                if (!ret.isOK()) {
                    return ret;
                }
            }
        } else if (iterator->_type == StringMap) {
            StringMap_t sourceValue;
            ret = source.get(iterator->_dottedName, &sourceValue);
            if (!ret.isOK() && ret != ErrorCodes::NoSuchKey) {
                StringBuilder sb;
                sb << "Error getting composable map value from source: " << ret.toString();
                return Status(ErrorCodes::InternalError, sb.str());
            }
            // Only do something if our source environment has something to add
            else if (ret.isOK()) {
                StringMap_t destValue;
                ret = dest->get(iterator->_dottedName, &destValue);
                if (!ret.isOK() && ret != ErrorCodes::NoSuchKey) {
                    StringBuilder sb;
                    sb << "Error getting composable map value from dest: " << ret.toString();
                    return Status(ErrorCodes::InternalError, sb.str());
                }

                // Iterate sourceValue and add elements to destValue
                for (StringMap_t::iterator sourceValueIt = sourceValue.begin();
                     sourceValueIt != sourceValue.end();
                     sourceValueIt++) {
                    destValue[sourceValueIt->first] = sourceValueIt->second;
                }

                // Set the resulting value in our output environment
                ret = dest->set(Key(iterator->_dottedName), Value(destValue));
                if (!ret.isOK()) {
                    return ret;
                }

                ret = canonicalizeOption(*iterator, dest);
                if (!ret.isOK()) {
                    return ret;
                }
            }
        } else {
            StringBuilder sb;
            sb << "Found composable option that is not of StringVector or "
               << "StringMap Type: " << iterator->_dottedName;
            return Status(ErrorCodes::InternalError, sb.str());
        }
    }

    return Status::OK();
}

/**
* For all options that have constraints, add those constraints to our environment so that
* they run when the environment gets validated.
*/
Status addConstraints(const OptionSection& options, Environment* dest) {
    std::vector<std::shared_ptr<Constraint>> constraints_vector;

    Status ret = options.getConstraints(&constraints_vector);
    if (!ret.isOK()) {
        return ret;
    }

    std::vector<std::shared_ptr<Constraint>>::const_iterator citerator;
    for (citerator = constraints_vector.begin(); citerator != constraints_vector.end();
         citerator++) {
        dest->addConstraint(citerator->get()).transitional_ignore();
    }

    return Status::OK();
}

/**
 *  Remove any options of type "Switch" that are set to false.  This is needed because boost
 *  defaults switches to false, and we need to be able to tell the difference between
 *  whether an option is set explicitly to false in config files or not present at all.
 */
Status removeFalseSwitches(const OptionSection& options, Environment* environment) {
    std::vector<OptionDescription> options_vector;
    Status ret = options.getAllOptions(&options_vector);
    if (!ret.isOK()) {
        return ret;
    }

    for (std::vector<OptionDescription>::const_iterator iterator = options_vector.begin();
         iterator != options_vector.end();
         iterator++) {
        if (iterator->_type == Switch) {
            bool switchValue;
            Status ret = environment->get(iterator->_dottedName, &switchValue);
            if (!ret.isOK() && ret != ErrorCodes::NoSuchKey) {
                StringBuilder sb;
                sb << "Error getting switch value for option: " << iterator->_dottedName
                   << " from source: " << ret.toString();
                return Status(ErrorCodes::InternalError, sb.str());
            } else if (ret.isOK() && switchValue == false) {
                Status ret = environment->remove(iterator->_dottedName);
                if (!ret.isOK()) {
                    StringBuilder sb;
                    sb << "Error removing false flag: " << iterator->_dottedName << ": "
                       << ret.toString();
                    return Status(ErrorCodes::InternalError, sb.str());
                }
            }
        }
    }

    return Status::OK();
}

}  // namespace

/**
 * This function delegates the command line parsing to boost program_options.
 *
 * 1. Extract the boost readable option descriptions and positional option descriptions from the
 * OptionSection
 * 2. Passes them to the boost command line parser
 * 3. Copy everything from the variables map returned by boost into the Environment
 */
Status OptionsParser::parseCommandLine(const OptionSection& options,
                                       const std::vector<std::string>& argv,
                                       Environment* environment) {
    po::options_description boostOptions;
    po::positional_options_description boostPositionalOptions;
    po::variables_map vm;

    // Need to convert to an argc and a vector of char* since that is what
    // boost::program_options expects as input to its command line parser
    int argc = 0;
    std::vector<const char*> argv_buffer;
    for (std::vector<std::string>::const_iterator iterator = argv.begin(); iterator != argv.end();
         iterator++) {
        argv_buffer.push_back(iterator->c_str());
        argc++;
    }

    // The function boost::program_options makes the assumption there is at
    // least one argument passed (usually the executable). When no options are
    // passed to the executable we're left with an argc value of 0 and an
    // empty argv_buffer vector from our post-processing above.
    //
    // This simply ensures that we always have at least one argument for
    // boost::program_options
    if (!argc) {
        argc = 1;
        argv_buffer.push_back(nullptr);
    }

    /**
     * Style options for boost command line parser
     *
     * unix_style is an alias for a group of basic style options.  We are deviating from that
     * base style in the following ways:
     *
     * 1. Don't allow guessing - '--dbpat' != '--dbpath'
     * 2. Don't allow sticky - '-hf' != '-h -f'
     * 3. Allow long disguises - '--dbpath' == '-dbpath'
     *
     * In some executables, we are using multiple 'v' options to set the verbosity (e.g. '-vvv')
     * To make this work, we need to allow long disguises and disallow guessing.
     */
    int style = (((po::command_line_style::unix_style ^ po::command_line_style::allow_guessing) |
                  po::command_line_style::allow_long_disguise) ^
                 po::command_line_style::allow_sticky);

    Status ret = options.getBoostOptions(&boostOptions, false, false, SourceCommandLine);
    if (!ret.isOK()) {
        return ret;
    }

    ret = options.getBoostPositionalOptions(&boostPositionalOptions);
    if (!ret.isOK()) {
        return ret;
    }

    try {
        po::store(po::command_line_parser(argc, &argv_buffer[0])
                      .options(boostOptions)
                      .positional(boostPositionalOptions)
                      .style(style)
                      .run(),
                  vm);

        ret = addBoostVariablesToEnvironment(vm, options, environment);
        if (!ret.isOK()) {
            return ret;
        }
    } catch (po::multiple_occurrences& e) {
        StringBuilder sb;
        sb << "Error parsing command line:  Multiple occurrences of option \""
           << e.get_option_name() << "\"";
        return Status(ErrorCodes::BadValue, sb.str());
    } catch (po::error& e) {
        StringBuilder sb;
        sb << "Error parsing command line: " << e.what();
        return Status(ErrorCodes::BadValue, sb.str());
    }

    // This is needed because "switches" default to false in boost, and we don't want to
    // erroneously think that they were present but set to false in a config file.
    ret = removeFalseSwitches(options, environment);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

/**
 * This function delegates the INI config parsing to boost program_options.
 *
 * 1. Extract the boost readable option descriptions from the OptionSection
 * 2. Passes them to the boost config file parser
 * 3. Copy everything from the variables map returned by boost into the Environment
 */
Status OptionsParser::parseINIConfigFile(const OptionSection& options,
                                         const std::string& config,
                                         Environment* environment) {
    po::options_description boostOptions;
    po::variables_map vm;

    Status ret = options.getBoostOptions(&boostOptions, false, false, SourceINIConfig);
    if (!ret.isOK()) {
        return ret;
    }

    std::istringstream is(config);
    try {
        po::store(po::parse_config_file(is, boostOptions, !OptionsParser::useStrict()), vm);
        ret = addBoostVariablesToEnvironment(vm, options, environment);
        if (!ret.isOK()) {
            return ret;
        }
    } catch (po::multiple_occurrences& e) {
        StringBuilder sb;
        sb << "Error parsing INI config file:  Multiple occurrences of option \""
           << e.get_option_name() << "\"";
        return Status(ErrorCodes::BadValue, sb.str());
    } catch (po::error& e) {
        StringBuilder sb;
        sb << "Error parsing INI config file: " << e.what();
        return Status(ErrorCodes::BadValue, sb.str());
    }
    return Status::OK();
}

namespace {

/**
 * This function delegates the YAML config parsing to the third party YAML parser.  It does no
 * error checking other than the parse error checking done by the YAML parser.
 */
Status parseYAMLConfigFile(const std::string& config, YAML::Node* YAMLConfig) {
    try {
        *YAMLConfig = YAML::Load(config);
    } catch (const YAML::Exception& e) {
        StringBuilder sb;
        sb << "Error parsing YAML config file: " << e.what();
        return Status(ErrorCodes::BadValue, sb.str());
    } catch (const std::runtime_error& e) {
        StringBuilder sb;
        sb << "Unexpected exception parsing YAML config file: " << e.what();
        return Status(ErrorCodes::BadValue, sb.str());
    }

    return Status::OK();
}

bool isYAMLConfig(const YAML::Node& config) {
    // The YAML parser is very forgiving, and for the INI config files we've parsed so far using
    // the YAML parser, the YAML parser just slurps the entire config file into a single string
    // rather than erroring out.  Thus, we assume that having a scalar (string) as the root node
    // means that this is not meant to be a YAML config file, since even a very simple YAML
    // config file should be parsed as a Map, and thus "config.IsScalar()" would return false.
    //
    // This requires more testing, both to ensure that all INI style files get parsed as a
    // single string, and to ensure that the user experience does not suffer (in terms of this
    // causing confusing error messages for users writing a brand new YAML config file that
    // incorrectly triggers this check).
    if (config.IsScalar()) {
        return false;
    } else {
        return true;
    }
}

}  // namespace

/**
 * Add default values from the given OptionSection to the given Environment
 */
Status OptionsParser::addDefaultValues(const OptionSection& options, Environment* environment) {
    std::map<Key, Value> defaultOptions;

    Status ret = options.getDefaults(&defaultOptions);
    if (!ret.isOK()) {
        return ret;
    }

    typedef std::map<Key, Value>::iterator it_type;
    for (it_type iterator = defaultOptions.begin(); iterator != defaultOptions.end(); iterator++) {
        ret = environment->setDefault(iterator->first, iterator->second);
        if (!ret.isOK()) {
            return ret;
        }
    }

    return Status::OK();
}

/**
 * Reads the entire config file into the output string.  This was done this way because the JSON
 * parser only takes complete strings, and we were using that to parse the config file before.
 * We could redesign the parser to use some kind of streaming interface, but for now this is
 * simple and works for the current use case of config files which should be limited in size.
 */
Status OptionsParser::readConfigFile(const std::string& filename, std::string* contents) {
    std::ifstream file;
    file.open(filename.c_str());
    if (file.fail()) {
        const int current_errno = errno;
        StringBuilder sb;
        sb << "Error opening config file: " << strerror(current_errno);
        return Status(ErrorCodes::InternalError, sb.str());
    }

    // check if it's a regular file
    fs::path configPath(filename);
    if (!fs::is_regular_file(filename)) {
        StringBuilder sb;
        sb << "Error opening config file: " << strerror(EISDIR);
        return Status(ErrorCodes::InternalError, sb.str());
    }

    // Transfer data to a stringstream
    std::stringstream config;
    std::string configString;
    try {
        config << file.rdbuf();
        configString = config.str();
    } catch (const std::exception& e) {
        StringBuilder sb;
        sb << "Error reading in config file: " << e.what();
        return Status(ErrorCodes::InternalError, sb.str());
    }

    // Config files cannot have null bytes
    if (std::count(configString.begin(), configString.end(), '\0') > 0) {

#if defined(_WIN32)
        // On Windows, it is common for files to be saved by Notepad as UTF-16 with a BOM so convert
        // it for the user. If the file lacks a BOM, but is UTF-16 encoded we will fail rather then
        // try to guess the file encoding.
        const std::array<unsigned char, 2> UTF16LEBOM = {0xff, 0xfe};
        if (configString.size() >= UTF16LEBOM.size() &&
            memcmp(configString.data(), UTF16LEBOM.data(), UTF16LEBOM.size()) == 0) {
            auto wstr = std::wstring(configString.begin() + 2, configString.end());
            *contents = toUtf8String(wstr);
            return Status::OK();
        }
#endif
        return Status(
            ErrorCodes::FailedToParse,
            "Config file has null bytes, ensure the file is saved as UTF-8 and not UTF-16.");
    }
    // Copy the vector contents into our result string
    *contents = std::move(configString);
    return Status::OK();
}

namespace {
/**
 * Find implicit options and merge them with "=".
 * Implicit options in boost 1.59 no longer support
 * --option value
 * instead they only support "--option=value", this function
 * attempts to workound this by translating the former into the later.
 */
StatusWith<std::vector<std::string>> transformImplicitOptions(
    const OptionSection& options, const std::vector<std::string>& argvOriginal) {
    if (argvOriginal.empty()) {
        return {std::vector<std::string>()};
    }

    std::vector<OptionDescription> optionDescs;
    Status ret = options.getAllOptions(&optionDescs);
    if (!ret.isOK()) {
        return ret;
    }

    std::map<std::string, const OptionDescription*> implicitOptions;
    for (const auto& opt : optionDescs) {
        if (opt._implicit.isEmpty()) {
            continue;
        }

        // Some options have a short name (a single letter) in addition to a long name.
        // In this case, the format for the name of the option is "long_name,S" where S is a
        // single character.
        // This is validated as such by the boost option parser later in the code.
        size_t pos = opt._singleName.find(',');
        if (pos != std::string::npos) {
            implicitOptions[opt._singleName.substr(0, pos)] = &opt;
            implicitOptions[opt._singleName.substr(pos + 1)] = &opt;
        } else {
            implicitOptions[opt._singleName] = &opt;
        }

        for (const std::string& deprecatedSingleName : opt._deprecatedSingleNames) {
            pos = deprecatedSingleName.find(',');
            if (pos != std::string::npos) {
                implicitOptions[deprecatedSingleName.substr(0, pos)] = &opt;
                implicitOptions[deprecatedSingleName.substr(pos + 1)] = &opt;
            } else {
                implicitOptions[deprecatedSingleName] = &opt;
            }
        }
    }

    std::vector<std::string> args;
    args.reserve(argvOriginal.size());

    // If there are no implicit options for this option parser instance, no filtering is needed.
    if (implicitOptions.empty()) {
        std::copy(argvOriginal.begin(), argvOriginal.end(), std::back_inserter(args));

        return {args};
    }

    // Now try to merge the implicit arguments
    // Candidates to merge:
    //   -arg value
    //   --arg value
    //   -arg ""
    //   --arg ""
    // Candidates not to merge:
    //   -arg=value
    //   --arg=value
    for (size_t i = 0; i < argvOriginal.size(); i++) {
        std::string arg = argvOriginal[i];

        // Enable us to fall through to the default code path of pushing another arg into the vector
        do {
            // Skip processing of the last argument in the array since there would be nothing to
            // merge it with
            if (i == argvOriginal.size() - 1) {
                break;
            }

            // Skip empty strings
            if (arg.empty()) {
                break;
            }

            // All options start with at least one "-"
            if (arg[0] == '-') {
                int argPrefix = 1;

                // Is the argument just a single "-", i.e. short form or a disguised long form?
                if (arg.size() == 1) {
                    break;
                }

                if (arg[argPrefix] == '-') {
                    // Is the argument just a double "--", i.e. long form?
                    if (arg.size() == 2) {
                        break;
                    }

                    ++argPrefix;
                }

                // Now we strip the prefix, and do a match on the parameter name.
                // At this point we may have "option=value" which is not in our option list so
                // we move onto the next option.
                std::string parameter = arg.substr(argPrefix);

                const auto iterator = implicitOptions.find(parameter);
                if (iterator == implicitOptions.end()) {
                    break;
                }

                // If the next argument is the empty string, just eat the empty string
                if (argvOriginal[i + 1].empty()) {
                    const auto& value = iterator->second->_implicit;
                    std::string defaultStr;

                    Status stringStatus = value.get(&defaultStr);

                    // If the type of the option was a string type with a implicit value other than
                    // "", then we cannot handle merging these two strings because simply leaving
                    // just the option name around has a different behavior.
                    // i.e., it is impossible to have "--option=''" on the command line as some
                    // non-empty string must follow the equal sign.
                    // This specific case is only known to affect "verbose" in the long form in
                    // mongod and mongos which makes it a breaking change for this one specific
                    // change. Users can get similar behavior by removing both the option and the
                    // original string in this case.
                    if (stringStatus.isOK() && !defaultStr.empty()) {
                        return Status(ErrorCodes::BadValue,
                                      "The empty string is not supported with the option " +
                                          parameter);
                    };

                    // For strings that have an implicit value of "", we just
                    // disregard the empty string argument since it has the same meaning.
                    // For other types like integers this is not a problem in non-test code as the
                    // only options with implicit options are string types.
                } else {
                    // Before we decide to merge the arguments, we must see if the next argument
                    // is actually an option. Boost checks its list of options, but we will just
                    // guess based on a leading '-' rather then try to match Boost's logic.
                    if (argvOriginal[i + 1][0] == '-') {
                        break;
                    }

                    if (parameter.size() == 1) {
                        // Merge this short form argument and the next into one.
                        // Note: we do not support allow_sticky so a simple append works
                        arg = str::stream() << argvOriginal[i] << argvOriginal[i + 1];
                    } else {
                        // Merge this long form argument and the next into one.
                        arg = str::stream() << argvOriginal[i] << "=" << argvOriginal[i + 1];
                    }
                }

                // Advance to the next argument
                i++;
            }
        } while (false);

        // Add the option to the final list, this may be an option that we simply ignore.
        args.push_back(arg);
    }

    return {args};
}

}  // namespace

StatusWith<OptionsParser::ConfigExpand> parseConfigExpand(const Environment& cli) {
    OptionsParser::ConfigExpand ret;

    if (!cli.count("configExpand")) {
        return ret;
    }

    Value expand_value;
    auto status = cli.get(Key("configExpand"), &expand_value);
    if (!status.isOK()) {
        return status;
    }

    auto expand = expand_value.as<std::string>();
    if (expand == "none") {
        return ret;
    }

    Value timeout_value;
    status = cli.get(Key("configExpandTimeoutSecs"), &timeout_value);
    if (status.isOK()) {
        auto timeout = timeout_value.as<int>();
        if (timeout <= 0) {
            return {ErrorCodes::BadValue, "Invalid value for configExpandTimeoutSecs"};
        }
        ret.timeout = Seconds{timeout};
    }

    StringData expandSD(expand);
    while (!expandSD.empty()) {
        StringData elem;
        auto comma = expandSD.find(',');
        if (comma == std::string::npos) {
            elem = expandSD;
            expandSD = StringData();
        } else {
            elem = expandSD.substr(0, comma);
            expandSD = expandSD.substr(comma + 1);
        }
        if (elem == "rest") {
            ret.rest = true;
        } else if (elem == "exec") {
            ret.exec = true;
        } else {
            return {ErrorCodes::BadValue,
                    str::stream() << "Invalid value for --configExpand: '" << elem << "'"};
        }
    }
    return ret;
}

/**
 * Run the OptionsParser
 *
 * Overview:
 *
 * 1. Parse argc and argv using the given OptionSection as a description of expected options
 * 2. Check for a "config" argument
 * 3. If "config" found, read config file
 * 4. Detect config file type (YAML or INI)
 * 5. Parse config file using the given OptionSection as a description of expected options
 * 6. Add the results to the output Environment in the proper order to ensure correct precedence
 */
Status OptionsParser::run(const OptionSection& options,
                          const std::vector<std::string>& argvOriginal,
                          const std::map<std::string, std::string>& env,  // XXX: Currently unused
                          Environment* environment) {
    Environment commandLineEnvironment;
    Environment configEnvironment;
    Environment composedEnvironment;

    auto swTransform = transformImplicitOptions(options, argvOriginal);
    if (!swTransform.isOK()) {
        return swTransform.getStatus();
    }

    std::vector<std::string> argvTransformed = std::move(swTransform.getValue());

    Status ret = parseCommandLine(options, argvTransformed, &commandLineEnvironment);
    if (!ret.isOK()) {
        return ret;
    }

    Value config_value;
    ret = commandLineEnvironment.get(Key("config"), &config_value);
    // We had an error other than "config" not existing in our environment
    if (!ret.isOK() && ret != ErrorCodes::NoSuchKey) {
        return ret;
    }

    // "config" exists in our environment
    else if (ret.isOK()) {
        // Environment::get returns a bad status if config was not set
        std::string config_filename;
        ret = config_value.get(&config_filename);
        if (!ret.isOK()) {
            return ret;
        }

        std::string config_file;
        ret = readConfigFile(config_filename, &config_file);
        if (!ret.isOK()) {
            return ret;
        }

        auto swExpand = parseConfigExpand(commandLineEnvironment);
        if (!swExpand.isOK()) {
            return swExpand.getStatus();
        }
        auto configExpand = std::move(swExpand.getValue());

        ret = parseConfigFile(options, config_file, &configEnvironment, configExpand);
        if (!ret.isOK()) {
            return ret;
        }
    }

    // Adds the values for all our options that were registered as composable to the composed
    // environment.  addCompositions doesn't override the values like "setAll" on our
    // environment.  Instead it aggregates the values in the result environment.
    // NOTE: We must add our configEnvironment compositions first since we have a StringMap type
    // in which some options can be overridden by the command line.
    ret = addCompositions(options, configEnvironment, &composedEnvironment);
    if (!ret.isOK()) {
        return ret;
    }

    ret = addCompositions(options, commandLineEnvironment, &composedEnvironment);
    if (!ret.isOK()) {
        return ret;
    }

    // Add the default values to our resulting environment
    ret = addDefaultValues(options, environment);
    if (!ret.isOK()) {
        return ret;
    }

    // Add the values to our result in the order of override
    // NOTE: This should not fail validation as we haven't called environment->validate() yet
    ret = environment->setAll(configEnvironment);
    if (!ret.isOK()) {
        return ret;
    }
    ret = environment->setAll(commandLineEnvironment);
    if (!ret.isOK()) {
        return ret;
    }

    // Add this last because it has all the composable options aggregated over different
    // sources.  For example, if we have a StringMap type with some values set on the command
    // line and some values set in config files, we want to make sure to get them all.  This
    // should not override any non composable options, since composedEnvironment should not have
    // them set.  See the addCompositions function for more details.
    ret = environment->setAll(composedEnvironment);
    if (!ret.isOK()) {
        return ret;
    }

    // Add the constraints from our options to the result environment
    ret = addConstraints(options, environment);
    if (!ret.isOK()) {
        return ret;
    }

    return Status::OK();
}

Status OptionsParser::runConfigFile(
    const OptionSection& options,
    const std::string& config,
    const std::map<std::string, std::string>& env,  // Unused, interface consistent with run()
    Environment* configEnvironment) {
    // Add the default values to our resulting environment
    Status ret = addDefaultValues(options, configEnvironment);
    if (!ret.isOK()) {
        return ret;
    }

    // Add values from the provided config file
    ret = parseConfigFile(options, config, configEnvironment, ConfigExpand());
    if (!ret.isOK()) {
        return ret;
    }

    // Add the constraints from our options to the result environment
    ret = addConstraints(options, configEnvironment);
    if (!ret.isOK()) {
        return ret;
    }

    return ret;
}

Status OptionsParser::parseConfigFile(const OptionSection& options,
                                      const std::string& config_file,
                                      Environment* configEnvironment,
                                      const ConfigExpand& configExpand) {
    YAML::Node YAMLConfig;
    Status ret = parseYAMLConfigFile(config_file, &YAMLConfig);
    if (!ret.isOK()) {
        return ret;
    }

    // Check if YAML parsing was successful, if not try to read as INI
    if (isYAMLConfig(YAMLConfig)) {
        ret = addYAMLNodesToEnvironment(YAMLConfig, options, "", configEnvironment, configExpand);
        if (!ret.isOK()) {
            return ret;
        }
    } else {
        ret = parseINIConfigFile(options, config_file, configEnvironment);
        if (!ret.isOK()) {
            return ret;
        }
    }

    return ret;
}

}  // namespace optionenvironment
}  // namespace mongo
