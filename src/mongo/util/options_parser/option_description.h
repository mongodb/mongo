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

#include <vector>

#include "mongo/base/status.h"
#include "mongo/util/options_parser/constraints.h"
#include "mongo/util/options_parser/value.h"

namespace mongo {
namespace optionenvironment {

/**
 * An OptionType is an enum of all the types we support in the OptionsParser
 *
 * NOTE(sverch): The semantics of "Switch" options are completely identical to "Bool" options,
 * except that on the command line they do not take a value.
 */
enum OptionType {
    StringVector,      // po::value< std::vector<std::string> >
    StringMap,         // po::value< std::vector<std::string> > (but in "key=value" format)
    Bool,              // po::value<bool>
    Double,            // po::value<double>
    Int,               // po::value<int>
    Long,              // po::value<long>
    String,            // po::value<std::string>
    UnsignedLongLong,  // po::value<unsigned long long>
    Unsigned,          // po::value<unsigned>
    Switch             // po::bool_switch
};

/**
 * An OptionSources is an enum representing where an option can come from
 */
enum OptionSources {
    SourceCommandLine = 1,
    SourceINIConfig = 2,
    SourceYAMLConfig = 4,
    SourceAllConfig = SourceINIConfig | SourceYAMLConfig,
    SourceAllLegacy = SourceINIConfig | SourceCommandLine,
    SourceAll = SourceCommandLine | SourceINIConfig | SourceYAMLConfig
};

/**
 * The OptionDescription class is a container for information about the options we are expecting
 * either on the command line or in config files.  These should be registered in an
 * OptionSection instance and passed to an OptionsParser.
 */
class OptionDescription {
public:
    OptionDescription(const std::string& dottedName,
                      const std::string& singleName,
                      const OptionType type,
                      const std::string& description);

    OptionDescription(const std::string& dottedName,
                      const std::string& singleName,
                      const OptionType type,
                      const std::string& description,
                      const std::vector<std::string>& deprecatedDottedNames);

    /*
     * The following functions are part of the chaining interface for option registration.  See
     * comments below for what each of these attributes mean, and the OptionSection class for
     * more details on the chaining interface.
     */

    /**
     * Parsing Attributes.
     *
     * The functions below specify various attributes of our option that are relevant for
     * parsing.
     */

    /*
     * Make this option hidden so it does not appear in command line help
     */
    OptionDescription& hidden();

    /*
     * Add a default value for this option if it is not specified
     *
     * throws DBException on errors, such as trying to set a default that does not have the same
     * type as the option, or trying to set a default for a composing option.
     */
    OptionDescription& setDefault(Value defaultValue);

    /*
     * Add an implicit value for this option if it is specified with no argument
     *
     * throws DBException on errors, such as trying to set an implicit value that does not have
     * the same type as the option, or trying to set an implicit value for a composing option.
     */
    OptionDescription& setImplicit(Value implicitValue);

    /*
     * Make this option composing so that the different sources add their values instead of
     * overriding (eg. setParameter values in the config file and on the command line all get
     * aggregated together)
     *
     * throws DBException on errors, such as trying to make an option that is not a vector type
     * composing, or or trying to set an implicit or default value for a composing option.
     */
    OptionDescription& composing();

    /*
     * Specify the allowed sources for this option, such as CommandLine, JSONConfig, or
     * INIConfig.  The default is SourceAll which means the option can be present in any source
     */
    OptionDescription& setSources(OptionSources sources);

    /*
     * Specify that this is a positional option.  "start" should be the first position the
     * option can be found in, and "end" is the last position, inclusive.  The positions start
     * at index 1 (after the executable name).  If "start" is greater than "end", then the
     * option must be able to support multiple values.  Specifying -1 for the "end" means that
     * the option can repeat forever.  Any "holes" in the positional ranges will result in an
     * error during parsing.
     *
     * Examples:
     *
     * .positional(1,1) // Single positional argument at position 1
     * ...
     * .positional(2,3) // More positional arguments at position 2 and 3 (multivalued option)
     * ...
     * .positional(4,-1) // Can repeat this positional option forever after position 4
     *
     *
     * (sverch) TODO: When we can support it (i.e. when we can get rid of boost) add a
     * "positionalOnly" attribute that specifies that it is not also a command line flag.  In
     * boost program options, the only way to have a positional argument is to register a flag
     * and mark it as also being positional.
     */
    OptionDescription& positional(int start, int end);

    /**
     * Validation Constraints.
     *
     * The functions below specify constraints that must be met in order for this option to be
     * valid.  These do not get checked during parsing, but will be added to the result
     * Environment so that they will get checked when the Environment is validated.
     */

    /**
     * Specifies the range allowed for this option.  Only allowed for options with numeric type.
     */
    OptionDescription& validRange(long min, long max);

    /**
     * Specifies that this option is incompatible with another option.  The std::string provided
     * must be the dottedName, which is the name used to access the option in the result
     * Environment.
     *
     * TODO: Find a way to check that that option actually exists in our section somewhere.
     */
    OptionDescription& incompatibleWith(const std::string& otherDottedName);

    /**
     * Specifies that this option is requires another option to be specified.  The string
     * provided must be the dottedName, which is the name used to access the option in the
     * result Environment.
     */
    OptionDescription& requires(const std::string& otherDottedName);

    /**
     * Specifies that this option is required to match the given format, specified as a regular
     * expression.  The displayFormat argument is what gets printed to the user in the case
     * where this constraint is not satisfied.  This is only allowed on std::string options.
     */
    OptionDescription& format(const std::string& regexFormat, const std::string& displayFormat);

    /**
     * Adds a constraint for this option.  During parsing, this Constraint will be added to the
     * result Environment, ensuring that it will get checked when the environment is validated.
     * See the documentation on the Constraint and Environment classes for more details.
     *
     * WARNING: This function takes ownership of the Constraint pointer that is passed in.
     */
    OptionDescription& addConstraint(Constraint* c);

    std::string _dottedName;   // Used for JSON config and in Environment
    std::string _singleName;   // Used for boost command line and INI
    OptionType _type;          // Storage type of the argument value, or switch type (bool)
                               // (required by boost)
    std::string _description;  // Description of option printed in help output
    bool _isVisible;           // Visible in help output
    Value _default;            // Value if option is not specified
    Value _implicit;           // Value if option is specified with no argument
    bool _isComposing;         // Aggregate values from different sources instead of overriding
    OptionSources _sources;    // Places where an option can be specified (current sources are
                               // command line, json config, and ini config)
    int _positionalStart;  // The starting position if this is a positional option. -1 otherwise.
    int _positionalEnd;    // The ending position if this is a positional option.  -1 if unlimited.

    // TODO(sverch): We have to use pointers to keep track of the Constrants because we rely on
    // inheritance to make Constraints work.  We have to use shared_ptrs because the
    // OptionDescription is sometimes copied and because it is stored in a std::list in the
    // OptionSection.  We should think about a better solution for the ownership semantics of
    // these classes.  Note that the Environment (the storage for results of option parsing) has
    // to know about the constraints for all the options, which is another factor to consider
    // when thinking about ownership.
    std::vector<std::shared_ptr<Constraint>> _constraints;  // Constraints that must be met
                                                            // for this option to be valid

    // Deprecated dotted names - aliases for '_dottedName'.
    std::vector<std::string> _deprecatedDottedNames;
};

}  // namespace optionenvironment
}  // namespace mongo
