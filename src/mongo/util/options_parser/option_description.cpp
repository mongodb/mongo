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

#include "mongo/util/options_parser/option_description.h"

#include <algorithm>

#include "mongo/util/assert_util.h"

namespace mongo {
namespace optionenvironment {

using std::shared_ptr;

namespace {
/**
 * Utility function check that the type of our Value matches our OptionType
 */
Status checkValueType(OptionType type, Value value) {
    switch (type) {
        case StringVector: {
            std::vector<std::string> valueType;
            return value.get(&valueType);
        }
        case Bool: {
            bool valueType;
            return value.get(&valueType);
        }
        case Double: {
            double valueType;
            return value.get(&valueType);
        }
        case Int: {
            int valueType;
            return value.get(&valueType);
        }
        case Long: {
            long valueType;
            return value.get(&valueType);
        }
        case String: {
            std::string valueType;
            return value.get(&valueType);
        }
        case UnsignedLongLong: {
            unsigned long long valueType;
            return value.get(&valueType);
        }
        case Unsigned: {
            unsigned valueType;
            return value.get(&valueType);
        }
        case Switch: {
            bool valueType;
            return value.get(&valueType);
        }
        default: {
            StringBuilder sb;
            sb << "Unrecognized option type: " << type;
            return Status(ErrorCodes::InternalError, sb.str());
        }
    }
}
}  // namespace

OptionDescription::OptionDescription(const std::string& dottedName,
                                     const std::string& singleName,
                                     const OptionType type,
                                     const std::string& description)
    : _dottedName(dottedName),
      _singleName(singleName),
      _type(type),
      _description(description),
      _isVisible(true),
      _default(Value()),
      _implicit(Value()),
      _isComposing(false),
      _sources(SourceAll),
      _positionalStart(-1),
      _positionalEnd(-1),
      _constraints(),
      _deprecatedDottedNames() {}

OptionDescription::OptionDescription(const std::string& dottedName,
                                     const std::string& singleName,
                                     const OptionType type,
                                     const std::string& description,
                                     const std::vector<std::string>& deprecatedDottedNames)
    : _dottedName(dottedName),
      _singleName(singleName),
      _type(type),
      _description(description),
      _isVisible(true),
      _default(Value()),
      _implicit(Value()),
      _isComposing(false),
      _sources(SourceAll),
      _positionalStart(-1),
      _positionalEnd(-1),
      _constraints(),
      _deprecatedDottedNames(deprecatedDottedNames) {
    // Verify deprecated dotted names.
    // No empty deprecated dotted names.
    if (std::count(_deprecatedDottedNames.begin(), _deprecatedDottedNames.end(), "")) {
        StringBuilder sb;
        sb << "Attempted to register option with empty string for deprecated dotted name";
        uasserted(ErrorCodes::BadValue, sb.str());
    }
    // Should not be the same as _dottedName.
    if (std::count(_deprecatedDottedNames.begin(), _deprecatedDottedNames.end(), dottedName)) {
        StringBuilder sb;
        sb << "Attempted to register option with conflict between dottedName and deprecated "
           << "dotted name: " << _dottedName;
        uasserted(ErrorCodes::BadValue, sb.str());
    }
}

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
        uasserted(ErrorCodes::InternalError, sb.str());
    }

    // Make sure the type of our default value matches our declared type
    Status ret = checkValueType(_type, defaultValue);
    if (!ret.isOK()) {
        StringBuilder sb;
        sb << "Could not register option \"" << _dottedName << "\": "
           << "mismatch between declared type and type of default value: " << ret.toString();
        uasserted(ErrorCodes::InternalError, sb.str());
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
        uasserted(ErrorCodes::InternalError, sb.str());
    }

    // Make sure the type of our implicit value matches our declared type
    Status ret = checkValueType(_type, implicitValue);
    if (!ret.isOK()) {
        StringBuilder sb;
        sb << "Could not register option \"" << _dottedName << "\": "
           << "mismatch between declared type and type of implicit value: " << ret.toString();
        uasserted(ErrorCodes::InternalError, sb.str());
    }

    // It doesn't make sense to set an "implicit value" for switch options since they can never
    // have an argument anyway, so disallow it here
    if (_type == Switch) {
        StringBuilder sb;
        sb << "Could not register option \"" << _dottedName << "\": "
           << "the implicit value of a Switch option is true and cannot be changed";
        uasserted(ErrorCodes::InternalError, sb.str());
    }

    _implicit = implicitValue;
    return *this;
}

OptionDescription& OptionDescription::composing() {
    if (_type != StringVector && _type != StringMap) {
        StringBuilder sb;
        sb << "Could not register option \"" << _dottedName << "\": "
           << "only options registered as StringVector or StringMap can be composing";
        uasserted(ErrorCodes::InternalError, sb.str());
    }

    // Disallow registering a default value for a composing option since the interaction
    // between the two is unclear
    if (!_default.isEmpty()) {
        StringBuilder sb;
        sb << "Could not register option \"" << _dottedName << "\": "
           << "Cannot make an option with an default value composing";
        uasserted(ErrorCodes::InternalError, sb.str());
    }

    // Disallow registering an implicit value for a composing option since the interaction
    // between the two is unclear
    if (!_implicit.isEmpty()) {
        StringBuilder sb;
        sb << "Could not register option \"" << _dottedName << "\": "
           << "Cannot make an option with an implicit value composing";
        uasserted(ErrorCodes::InternalError, sb.str());
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
        uasserted(ErrorCodes::InternalError, sb.str());
    }

    if ((end - start) > 0) {
        if (_type != StringVector) {
            StringBuilder sb;
            sb << "Could not register option \"" << _dottedName << "\": "
               << "Positional range implies that multiple values are allowed, "
               << "but option is not registered as type StringVector";
            uasserted(ErrorCodes::InternalError, sb.str());
        }
    }

    _positionalStart = start;
    _positionalEnd = end;
    return *this;
}

OptionDescription& OptionDescription::addConstraint(Constraint* c) {
    _constraints.push_back(std::shared_ptr<Constraint>(c));
    return *this;
}

OptionDescription& OptionDescription::validRange(long min, long max) {
    if (_type != Double && _type != Int && _type != Long && _type != UnsignedLongLong &&
        _type != Unsigned) {
        StringBuilder sb;
        sb << "Could not register option \"" << _dottedName << "\": "
           << "only options registered as a numeric type can have a valid range, "
           << "but option has type: " << _type;
        uasserted(ErrorCodes::InternalError, sb.str());
    }

    return addConstraint(new NumericKeyConstraint(_dottedName, min, max));
}

OptionDescription& OptionDescription::incompatibleWith(const std::string& otherDottedName) {
    return addConstraint(new MutuallyExclusiveKeyConstraint(_dottedName, otherDottedName));
}

OptionDescription& OptionDescription::requires(const std::string& otherDottedName) {
    return addConstraint(new RequiresOtherKeyConstraint(_dottedName, otherDottedName));
}

OptionDescription& OptionDescription::format(const std::string& regexFormat,
                                             const std::string& displayFormat) {
    if (_type != String) {
        StringBuilder sb;
        sb << "Could not register option \"" << _dottedName << "\": "
           << "only options registered as a string type can have a required format, "
           << "but option has type: " << _type;
        uasserted(ErrorCodes::InternalError, sb.str());
    }

    return addConstraint(new StringFormatKeyConstraint(_dottedName, regexFormat, displayFormat));
}

}  // namespace optionenvironment
}  // namespace mongo
