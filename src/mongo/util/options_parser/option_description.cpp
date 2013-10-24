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

#include "mongo/util/options_parser/option_description.h"

#include "mongo/util/assert_util.h"

namespace mongo {
namespace optionenvironment {

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

    OptionDescription& OptionDescription::hidden() {
        _isVisible = false;
        return *this;
    }

    OptionDescription& OptionDescription::setDefault(Value defaultValue) {

        // Disallow registering a default for a composing option since the interaction between the
        // two is unclear (for example, should we override or compose the default)
        if (_isComposing) {
            StringBuilder sb;
            sb << "Could not register option \"" << _dottedName << "\": "
               << "Cannot register a default value for a composing option";
            throw DBException(sb.str(), ErrorCodes::InternalError);
        }

        // Make sure the type of our default value matches our declared type
        Status ret = checkValueType(_type, defaultValue);
        if (!ret.isOK()) {
            StringBuilder sb;
            sb << "Could not register option \"" << _dottedName << "\": "
            << "mismatch between declared type and type of default value: "
            << ret.toString();
            throw DBException(sb.str(), ErrorCodes::InternalError);
        }

        _default = defaultValue;
        return *this;
    }

    OptionDescription& OptionDescription::setImplicit(Value implicitValue) {

        // Disallow registering an implicit value for a composing option since the interaction
        // between the two is unclear
        if (_isComposing) {
            StringBuilder sb;
            sb << "Could not register option \"" << _dottedName << "\": "
               << "Cannot register an implicit value for a composing option";
            throw DBException(sb.str(), ErrorCodes::InternalError);
        }

        // Make sure the type of our implicit value matches our declared type
        Status ret = checkValueType(_type, implicitValue);
        if (!ret.isOK()) {
            StringBuilder sb;
            sb << "Could not register option \"" << _dottedName << "\": "
            << "mismatch between declared type and type of implicit value: "
            << ret.toString();
            throw DBException(sb.str(), ErrorCodes::InternalError);
        }

        _implicit = implicitValue;
        return *this;
    }

    OptionDescription& OptionDescription::composing() {

        if (_type != StringVector) {
            StringBuilder sb;
            sb << "Could not register option \"" << _dottedName << "\": "
                << "only options registered as StringVector can be composing";
            throw DBException(sb.str(), ErrorCodes::InternalError);
        }

        // Disallow registering a default value for a composing option since the interaction
        // between the two is unclear
        if (!_default.isEmpty()) {
            StringBuilder sb;
            sb << "Could not register option \"" << _dottedName << "\": "
               << "Cannot make an option with an default value composing";
            throw DBException(sb.str(), ErrorCodes::InternalError);
        }

        // Disallow registering an implicit value for a composing option since the interaction
        // between the two is unclear
        if (!_implicit.isEmpty()) {
            StringBuilder sb;
            sb << "Could not register option \"" << _dottedName << "\": "
               << "Cannot make an option with an implicit value composing";
            throw DBException(sb.str(), ErrorCodes::InternalError);
        }

        _isComposing = true;
        return *this;
    }

    OptionDescription& OptionDescription::setSources(OptionSources sources) {
        _sources = sources;
        return *this;
    }

    OptionDescription& OptionDescription::positional(int start, int end) {

        if (start < 1 || (end < 1 && end != -1) || (end != -1 && end < start)) {
            StringBuilder sb;
            sb << "Could not register option \"" << _dottedName << "\": "
               << "Invalid positional specification:  \"start\": " << start << ", \"end\": " << end;
            throw DBException(sb.str(), ErrorCodes::InternalError);
        }

        if ((end - start) > 0) {
            if (_type != StringVector) {
                StringBuilder sb;
                sb << "Could not register option \"" << _dottedName << "\": "
                   << "Positional range implies that multiple values are allowed, "
                   << "but option is not registered as type StringVector";
                throw DBException(sb.str(), ErrorCodes::InternalError);
            }
        }

        _positionalStart = start;
        _positionalEnd = end;
        return *this;
    }

    OptionDescription& OptionDescription::addConstraint(Constraint* c) {
        _constraints.push_back(boost::shared_ptr<Constraint>(c));
        return *this;
    }

    OptionDescription& OptionDescription::validRange(long min, long max) {
        if (_type != Double &&
            _type != Int &&
            _type != Long &&
            _type != UnsignedLongLong &&
            _type != Unsigned) {
            StringBuilder sb;
            sb << "Could not register option \"" << _dottedName << "\": "
               << "only options registered as a numeric type can have a valid range, "
               << "but option has type: " << _type;
            throw DBException(sb.str(), ErrorCodes::InternalError);
        }

        return addConstraint(new NumericKeyConstraint(_dottedName, min, max));
    }

} // namespace optionenvironment
} // namespace mongo
