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

#include "mongo/util/options_parser/options_parser.h"

#include <boost/program_options.hpp>
#include <boost/shared_ptr.hpp>
#include <cerrno>
#include <fstream>
#include <stdio.h>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/constraints.h"
#include "mongo/util/options_parser/option_description.h"
#include "mongo/util/options_parser/option_section.h"

namespace mongo {
namespace optionenvironment {

    using namespace std;

    namespace po = boost::program_options;

    namespace {

        // The following section contains utility functions that convert between the various objects
        // we need to deal with while parsing command line options.
        //
        // These conversions are different depending on the data source because our current
        // implementation uses boost::program_options for the command line and INI files and the
        // mongo JSON parser for JSON config files.  Our destination storage in both cases is an
        // Environment which stores Value objects.
        //
        // 1. JSON Config Files
        //     The mongo JSON parser parses a JSON string into a BSON object.  Therefore, we need:
        //         a. A function to convert a BSONElement to a Value (BSONElementToValue)
        //         b. A function to iterate a BSONObj, convert the BSONElements to Values, and add
        //            them to our Environment (addBSONElementsToEnvironment)
        //
        // 2. INI Config Files and command line
        //     The boost::program_options parsers store their output in a
        //     boost::program_options::variables_map.  Therefore, we need:
        //         a. A function to convert a boost::any to a Value (boostAnyToValue)
        //         b. A function to iterate a variables_map, convert the boost::any elements to
        //            Values, and add them to our Environment (addBoostVariablesToEnvironment)

        // Convert a boost::any to a Value.  See comments at the beginning of this section.
        Status boostAnyToValue(const boost::any& anyValue, Value* value) {
            try {
                if (anyValue.type() == typeid(std::vector<std::string>)) {
                    *value = Value(boost::any_cast<std::vector<std::string> >(anyValue));
                }
                else if (anyValue.type() == typeid(bool)) {
                    *value = Value(boost::any_cast<bool>(anyValue));
                }
                else if (anyValue.type() == typeid(double)) {
                    *value = Value(boost::any_cast<double>(anyValue));
                }
                else if (anyValue.type() == typeid(int)) {
                    *value = Value(boost::any_cast<int>(anyValue));
                }
                else if (anyValue.type() == typeid(long)) {
                    *value = Value(boost::any_cast<long>(anyValue));
                }
                else if (anyValue.type() == typeid(std::string)) {
                    *value = Value(boost::any_cast<std::string>(anyValue));
                }
                else if (anyValue.type() == typeid(unsigned long long)) {
                    *value = Value(boost::any_cast<unsigned long long>(anyValue));
                }
                else if (anyValue.type() == typeid(unsigned)) {
                    *value = Value(boost::any_cast<unsigned>(anyValue));
                }
                else {
                    StringBuilder sb;
                    sb << "Unrecognized type: " << anyValue.type().name() <<
                        " in any to Value conversion";
                    return Status(ErrorCodes::InternalError, sb.str());
                }
            }
            catch(const boost::bad_any_cast& e) {
                StringBuilder sb;
                // We already checked the type, so this is just a sanity check
                sb << "boost::any_cast threw exception: " << e.what();
                return Status(ErrorCodes::InternalError, sb.str());
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

            for(std::vector<OptionDescription>::const_iterator iterator = options_vector.begin();
                iterator != options_vector.end(); iterator++) {

                // Trim off the short option from our name so we can look it up correctly in our map
                std::string long_name;
                std::string::size_type commaOffset = iterator->_singleName.find(',');
                if (commaOffset != string::npos) {
                    if (commaOffset != iterator->_singleName.size() - 2) {
                        StringBuilder sb;
                        sb << "Unexpected comma in option name: \"" << iterator->_singleName << "\""
                           << ": option name must be in the format \"option,o\" or \"option\", "
                           << "where \"option\" is the long name and \"o\" is the optional one "
                           << "character short alias";
                        return Status(ErrorCodes::BadValue, sb.str());
                    }
                    long_name = iterator->_singleName.substr(0, commaOffset);
                } else {
                    long_name = iterator->_singleName;
                }

                if (vm.count(long_name)) {
                    Value optionValue;
                    Status ret = boostAnyToValue(vm[long_name].value(), &optionValue);
                    if (!ret.isOK()) {
                        return ret;
                    }

                    // XXX: Don't set switches that are false, to maintain backwards compatibility
                    // with the old behavior during the transition to the new parser
                    if (iterator->_type == Switch) {
                        bool value;
                        ret = optionValue.get(&value);
                        if (!ret.isOK()) {
                            return ret;
                        }
                        if (!value) {
                            continue;
                        }
                    }

                    environment->set(iterator->_dottedName, optionValue);
                }
            }
            return Status::OK();
        }

        // Check if the given key is registered in our OptionDescription.  This is needed for JSON
        // Config File handling since the JSON parser just reads in whatever fields and values it
        // sees without taking a description of what to look for.
        Status isRegistered(const std::vector<OptionDescription>& options_vector, const Key& key) {

            for(std::vector<OptionDescription>::const_iterator iterator = options_vector.begin();
                iterator != options_vector.end(); iterator++) {
                if (key == iterator->_dottedName) {
                    return Status::OK();
                }
            }
            StringBuilder sb;
            sb << "Unrecognized option: " << key;
            return Status(ErrorCodes::BadValue, sb.str());
        }

        // Convert a BSONElement to a Value.  See comments at the beginning of this section.
        Status BSONElementToValue(const BSONElement& element, Value* value) {

            std::vector<BSONElement> elements;
            std::vector<std::string> valueStrings;
            try {
                switch (element.type()) {
                    case ::mongo::NumberInt:
                        *value = Value(element.Int());
                        return Status::OK();
                    case ::mongo::NumberDouble:
                        *value = Value(element.Double());
                        return Status::OK();
                    case ::mongo::NumberLong:
                        // FIXME: Figure out how to stop this, or detect overflow
                        *value = Value(static_cast<unsigned long long>(element.Long()));
                        return Status::OK();
                    case ::mongo::String:
                        *value = Value(element.String());
                        return Status::OK();
                    case ::mongo::Array:
                        elements = element.Array();
                        for(std::vector<BSONElement>::const_iterator iterator = elements.begin();
                            iterator != elements.end(); iterator++) {
                            if (iterator->type() == ::mongo::String) {
                                valueStrings.push_back(iterator->String());
                            }
                            else {
                                StringBuilder sb;
                                sb << "Arrays can only contain strings in JSON Config File";
                                return Status(ErrorCodes::BadValue, sb.str());
                            }
                        }
                        *value = Value(valueStrings);
                        return Status::OK();
                    case ::mongo::Bool:
                        *value = Value(element.Bool());
                        return Status::OK();
                    case ::mongo::EOO:
                        return Status(ErrorCodes::InternalError,
                                    "Error converting BSONElement to value; BSONElement empty");
                    default:
                        StringBuilder sb;
                        sb << "Conversion from BSONElement type: " <<
                            element.type() << " not supported.";
                        return Status(ErrorCodes::TypeMismatch, sb.str());
                }
            }
            catch ( std::exception &e ) {
                StringBuilder sb;
                sb << "Exception thrown by BSON conversion: " << e.what();
                return Status(ErrorCodes::InternalError, sb.str());
            }
        }

        // Add all the values in the given BSONObj to our environment.  See comments at the
        // beginning of this section.
        Status addBSONElementsToEnvironment(const BSONObj& obj,
                                             const OptionSection& options,
                                             const std::string parentPath,
                                             Environment* environment) {

            std::vector<OptionDescription> options_vector;
            Status ret = options.getAllOptions(&options_vector);
            if (!ret.isOK()) {
                return ret;
            }

            BSONObjIterator iterator(obj);
            while (iterator.more()) {
                BSONElement elem = iterator.next();
                string fieldName= elem.fieldName();

                std::string dottedName = ( parentPath.empty() ? fieldName
                                                              : parentPath+'.'+fieldName );

                if (elem.type() == ::mongo::Object) {
                    addBSONElementsToEnvironment( elem.Obj(), options, dottedName, environment );
                }
                else {
                    Value optionValue;
                    Status ret = BSONElementToValue(elem, &optionValue);
                    if (!ret.isOK()) {
                        return ret;
                    }

                    ret = isRegistered(options_vector, dottedName);
                    if (!ret.isOK()) {
                        return ret;
                    }

                    Value dummyVal;
                    if (environment->get(dottedName, &dummyVal).isOK()) {
                        StringBuilder sb;
                        sb << "Error parsing JSON config: duplcate key: " << dottedName;
                        return Status(ErrorCodes::BadValue, sb.str());
                    }

                    ret = environment->set(dottedName, optionValue);
                    if (!ret.isOK()) {
                        return ret;
                    }
                }
            }

            return Status::OK();
        }

        // Iterate through our options and add type constraints to our environment based on what
        // types the options were registered with.  This is needed for the JSON config file
        // handling, since the JSON parser just reads the types without checking them.  Currently,
        // the boost parsers check the types for us.
        Status addTypeConstraints(const OptionSection& options, Environment* environment) {

            std::vector<OptionDescription> options_vector;
            Status ret = options.getAllOptions(&options_vector);
            if (!ret.isOK()) {
                return ret;
            }

            for(std::vector<OptionDescription>::const_iterator iterator = options_vector.begin();
                iterator != options_vector.end(); iterator++) {
                switch (iterator->_type) {
                case StringVector:
                    environment->addKeyConstraint(
                           new TypeKeyConstraint<std::vector<std::string> >(iterator->_dottedName));
                    break;
                case Bool:
                    environment->addKeyConstraint(
                           new TypeKeyConstraint<bool>(iterator->_dottedName));
                    break;
                case Double:
                    environment->addKeyConstraint(
                           new TypeKeyConstraint<double>(iterator->_dottedName));
                    break;
                case Int:
                    environment->addKeyConstraint(
                           new TypeKeyConstraint<int>(iterator->_dottedName));
                    break;
                case Long:
                    environment->addKeyConstraint(
                           new TypeKeyConstraint<long>(iterator->_dottedName));
                    break;
                case String:
                    environment->addKeyConstraint(
                           new TypeKeyConstraint<std::string>(iterator->_dottedName));
                    break;
                case UnsignedLongLong:
                    environment->addKeyConstraint(
                           new TypeKeyConstraint<unsigned long long>(iterator->_dottedName));
                    break;
                case Unsigned:
                    environment->addKeyConstraint(
                           new TypeKeyConstraint<unsigned>(iterator->_dottedName));
                    break;
                case Switch:
                    environment->addKeyConstraint(
                           new TypeKeyConstraint<bool>(iterator->_dottedName));
                    break;
                default: /* XXX: should not get here */
                    return Status(ErrorCodes::InternalError, "Unrecognized option type");
                }
            }
            return Status::OK();
        }

        /**
        * For all options that we registered as composable, combine the values from source and dest
        * and set the result in dest.  Note that this only works for options that are registered as
        * vectors of strings.
        */
        Status addCompositions(const OptionSection& options,
                               const Environment& source,
                               Environment* dest) {
            std::vector<OptionDescription> options_vector;
            Status ret = options.getAllOptions(&options_vector);
            if (!ret.isOK()) {
                return ret;
            }

            for(std::vector<OptionDescription>::const_iterator iterator = options_vector.begin();
                iterator != options_vector.end(); iterator++) {
                if (iterator->_isComposing) {
                    std::vector<std::string> source_value;
                    ret = source.get(iterator->_dottedName, &source_value);
                    if (!ret.isOK() && ret != ErrorCodes::NoSuchKey) {
                        StringBuilder sb;
                        sb << "Error getting composable vector value from source: "
                            << ret.toString();
                        return Status(ErrorCodes::InternalError, sb.str());
                    }
                    // Only do something if our source environment has something to add
                    else if (ret.isOK()) {
                        std::vector<std::string> dest_value;
                        ret = dest->get(iterator->_dottedName, &dest_value);
                        if (!ret.isOK() && ret != ErrorCodes::NoSuchKey) {
                            StringBuilder sb;
                            sb << "Error getting composable vector value from dest: "
                                << ret.toString();
                            return Status(ErrorCodes::InternalError, sb.str());
                        }

                        // Append source_value on the end of dest_value
                        dest_value.insert(dest_value.end(),
                                          source_value.begin(),
                                          source_value.end());

                        // Set the resulting value in our output environment
                        ret = dest->set(Key(iterator->_dottedName), Value(dest_value));
                        if (!ret.isOK()) {
                            return ret;
                        }
                    }
                }
            }

            return Status::OK();
        }

    } // namespace

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
        for (std::vector<std::string>::const_iterator iterator = argv.begin();
            iterator != argv.end(); iterator++) {
            argv_buffer.push_back(iterator->c_str());
            argc++;
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
        int style = (((po::command_line_style::unix_style ^
                       po::command_line_style::allow_guessing) |
                      po::command_line_style::allow_long_disguise) ^
                     po::command_line_style::allow_sticky);

        Status ret = options.getBoostOptions(&boostOptions);
        if (!ret.isOK()) {
            return ret;
        }

        ret = options.getBoostPositionalOptions(&boostPositionalOptions);
        if (!ret.isOK()) {
            return ret;
        }

        try {
            po::store(po::command_line_parser(argc, &argv_buffer[0]).
                      options(boostOptions).
                      positional(boostPositionalOptions).
                      style(style).
                      run(), vm);

            ret = addBoostVariablesToEnvironment(vm, options, environment);
            if (!ret.isOK()) {
                return ret;
            }
        }
        catch (po::error& e) {
            StringBuilder sb;
            sb << "Error parsing command line: " << e.what();
            return Status(ErrorCodes::BadValue, sb.str());
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

        Status ret = options.getBoostOptions(&boostOptions);
        if (!ret.isOK()) {
            return ret;
        }

        std::istringstream is(config);
        try {
            po::store(po::parse_config_file(is, boostOptions), vm);
            ret = addBoostVariablesToEnvironment(vm, options, environment);
            if (!ret.isOK()) {
                return ret;
            }
        }
        catch (po::error& e) {
            StringBuilder sb;
            sb << "Error parsing INI config file: " << e.what();
            return Status(ErrorCodes::BadValue, sb.str());
        }
        return Status::OK();
    }

    /**
     * This function delegates the JSON config parsing to the MongoDB JSON parser.
     *
     * 1. Parse JSON
     * 2. Add all elements from the resulting BSONObj to the Environment
     *
     * This function checks for duplicates and unregistered options, but the caller is responsible
     * for checking that the options are the correct types
     *
     * Also note that the size of our JSON config file is limited in size.  The equivalent BSON
     * object can only be 16MB.  We catch the exception that is thrown in this case and return an
     * error Status from this function
     */
    Status OptionsParser::parseJSONConfigFile(const OptionSection& options,
                                              const std::string& config,
                                              Environment* environment) {
        BSONObj BSONConfig;
        try {
            BSONConfig = fromjson(config);
            Status ret = addBSONElementsToEnvironment(BSONConfig, options, "", environment);
            if (!ret.isOK()) {
                return ret;
            }
        } catch ( MsgAssertionException& e ) {
            StringBuilder sb;
            sb << "Error parsing JSON config file: " << e.what();
            return Status(ErrorCodes::BadValue, sb.str());
        }
        return Status::OK();
    }

    /**
     * Add default values from the given OptionSection to the given Environment
     */
    Status OptionsParser::addDefaultValues(const OptionSection& options,
                                           Environment* environment) {
        std::map <Key, Value> defaultOptions;

        Status ret = options.getDefaults(&defaultOptions);
        if (!ret.isOK()) {
            return ret;
        }

        typedef std::map<Key, Value>::iterator it_type;
        for(it_type iterator = defaultOptions.begin();
            iterator != defaultOptions.end(); iterator++) {
            ret = environment->setDefault(iterator->first, iterator->second);
            if (!ret.isOK()) {
                return ret;
            }
        }

        return Status::OK();
    }

    /**
     * Reads the entire config file into the output string.  This is done this way because the JSON
     * parser only takes complete strings
     */
    Status OptionsParser::readConfigFile(const std::string& filename, std::string* contents) {

        FILE* config;
        config = fopen(filename.c_str(), "r");
        if (config == NULL) {
            const int current_errno = errno;
            StringBuilder sb;
            sb << "Error reading config file: " << strerror(current_errno);
            return Status(ErrorCodes::InternalError, sb.str());
        }

        // Get length of config file by seeking to the end and getting the cursor position
        if (fseek(config, 0L, SEEK_END) != 0) {
            const int current_errno = errno;
            // TODO: Make sure errno is the correct way to do this
            // Confirmed that errno gets set in Mac OSX, but not documented
            StringBuilder sb;
            sb << "Error seeking in config file: " << strerror(current_errno);
            return Status(ErrorCodes::InternalError, sb.str());
        }
        long configSize = ftell(config);

        // Seek back to the beginning of the file for reading
        if (fseek(config, 0L, SEEK_SET) != 0) {
            const int current_errno = errno;
            // TODO: Make sure errno is the correct way to do this
            // Confirmed that errno gets set in Mac OSX, but not documented
            StringBuilder sb;
            sb << "Error seeking in config file: " << strerror(current_errno);
            return Status(ErrorCodes::InternalError, sb.str());
        }

        // Read into a vector first since it's guaranteed to have contiguous storage
        std::vector<char> configVector;
        configVector.resize(configSize);

        if (configSize > 0) {
            long nread = 0;
            while (!feof(config) && nread < configSize) {
                nread = nread + fread(&configVector[nread], sizeof(char),
                                      configSize - nread, config);
                if (ferror(config)) {
                    const int current_errno = errno;
                    // TODO: Make sure errno is the correct way to do this
                    StringBuilder sb;
                    sb << "Error reading in config file: " << strerror(current_errno);
                    return Status(ErrorCodes::InternalError, sb.str());
                }
            }
            // Resize our config vector to the number of bytes we actually read
            configVector.resize(nread);
        }

        // Copy the vector contents into our result string
        *contents = std::string(configVector.begin(), configVector.end());
        fclose(config);

        return Status::OK();
    }

    bool OptionsParser::isJSONConfig(const std::string& config) {
        for (std::string::const_iterator curChar = config.begin();
             curChar < config.end(); curChar++) {
            if (isspace(*curChar)) {
                // Skip whitespace
            }
            else if (*curChar == '{') {
                // If first non whitespace character is '{', then this is a JSON config file
                return true;
            }
            else {
                // Otherwise, this is a legacy INI config file
                return false;
            }
        }
        // Treat the empty config file as INI
        return false;
    }

    /**
     * Run the OptionsParser
     *
     * Overview:
     *
     * 1. Parse argc and argv using the given OptionSection as a description of expected options
     * 2. Check for a "config" argument
     * 3. If "config" found, read config file
     * 4. Detect config file type (JSON or INI)
     * 5. Parse config file using the given OptionSection as a description of expected options
     * 6. Add the results to the output Environment in the proper order to ensure correct precedence
     */
    Status OptionsParser::run(const OptionSection& options,
            const std::vector<std::string>& argv,
            const std::map<std::string, std::string>& env, // XXX: Currently unused
            Environment* environment) {

        Environment commandLineEnvironment;
        Environment configEnvironment;
        Environment composedEnvironment;

        Status ret = parseCommandLine(options, argv, &commandLineEnvironment);
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

            if (isJSONConfig(config_file)) {
                ret = parseJSONConfigFile(options, config_file, &configEnvironment);
                if (!ret.isOK()) {
                    return ret;
                }
                ret = addTypeConstraints(options, &configEnvironment);
                if (!ret.isOK()) {
                    return ret;
                }
                ret = configEnvironment.validate();
                if (!ret.isOK()) {
                    return ret;
                }
            }
            else {
                ret = parseINIConfigFile(options, config_file, &configEnvironment);
                if (!ret.isOK()) {
                    return ret;
                }
            }
        }

        // Adds the values for all our options that were registered as composable to the composed
        // environment.  addCompositions doesn't override the values like "setAll" on our
        // environment.  Instead it aggregates the values in the result environment.
        ret = addCompositions(options, commandLineEnvironment, &composedEnvironment);
        if (!ret.isOK()) {
            return ret;
        }

        ret = addCompositions(options, configEnvironment, &composedEnvironment);
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

        // Add this last because it represents the aggregated results of composing all environments
        ret = environment->setAll(composedEnvironment);
        if (!ret.isOK()) {
            return ret;
        }

        return Status::OK();
    }

} // namespace optionenvironment
} // namespace mongo
