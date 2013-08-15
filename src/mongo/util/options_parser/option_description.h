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

#include <iostream>

#include "mongo/base/status.h"
#include "mongo/util/options_parser/value.h"

namespace mongo {
namespace optionenvironment {

    /**
     * An OptionType is an enum of all the types we support in the OptionsParser
     */
    enum OptionType {
        StringVector,     // po::value< std::vector<std::string> >
        Bool,             // po::value<bool>
        Double,           // po::value<double>
        Int,              // po::value<int>
        Long,             // po::value<long>
        String,           // po::value<std::string>
        UnsignedLongLong, // po::value<unsigned long long>
        Unsigned,         // po::value<unsigned>
        Switch            // po::bool_switch
    };

    /**
     * The OptionDescription and PositionalOptionDescription classes are containers for information
     * about the options we are expecting either on the command line or in config files.  These
     * should be registered in an OptionSection instance and passed to an OptionsParser.
     */
    class OptionDescription {
    public:
        OptionDescription(const std::string& dottedName, // Used for JSON config and in Environment
                const std::string& singleName, // Used for boost command line and INI
                const OptionType type,
                const std::string& description,
                const bool isVisible = true,
                const Value defaultValue = Value(),
                const Value implicitValue = Value(),
                const bool isComposing = false)
            : _dottedName(dottedName),
            _singleName(singleName),
            _type(type),
            _description(description),
            _isVisible(isVisible),
            _default(defaultValue),
            _implicit(implicitValue),
            _isComposing(isComposing) { }

        std::string _dottedName;
        std::string _singleName;
        OptionType _type;
        std::string _description;
        bool _isVisible;
        Value _default;
        Value _implicit;
        bool _isComposing;
    };

    class PositionalOptionDescription {
    public:
        PositionalOptionDescription(const std::string& name,
                const OptionType type,
                int count = 1)
            : _name(name),
                _type(type),
                _count(count) { }

        std::string _name;
        OptionType _type;
        int _count; // Max number of times this option could be specified.  -1 = unlimited
    };

} // namespace optionenvironment
} // namespace mongo
