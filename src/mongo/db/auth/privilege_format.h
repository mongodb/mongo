// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * How user management functions should structure the BSON representation of privileges and roles.
 */
enum class PrivilegeFormat {
    kOmit,               // Privileges should not be included in the BSON representation.
    kShowSeparate,       // Privileges should be included, each as a separate entry.
    kShowAsUserFragment  // Privileges and roles should all be collapsed together, and presented as
                         // a fragment of a user document.
};

namespace auth {
using namespace std::literals::string_view_literals;

/**
 * Proxy for PrivilegeFormat to parse into and out of IDL formats.
 */
class ParsedPrivilegeFormat {
public:
    static constexpr std::string_view kAsUserFragment = "asUserFragment"sv;

    static PrivilegeFormat fromBool(bool fmt) {
        return fmt ? PrivilegeFormat::kShowSeparate : PrivilegeFormat::kOmit;
    }

    ParsedPrivilegeFormat() : _format(PrivilegeFormat::kOmit) {}
    explicit ParsedPrivilegeFormat(bool fmt) : _format(fromBool(fmt)) {}
    ParsedPrivilegeFormat(PrivilegeFormat fmt) : _format(fmt) {}
    ParsedPrivilegeFormat& operator=(bool fmt) {
        _format = fromBool(fmt);
        return *this;
    }

    PrivilegeFormat operator*() const {
        return _format;
    }

    static ParsedPrivilegeFormat parseFromBSON(const BSONElement& elem) {
        if (elem.eoo()) {
            return ParsedPrivilegeFormat();
        }
        if (elem.isNumber() || elem.isBoolean()) {
            return ParsedPrivilegeFormat(elem.trueValue());
        }
        if ((elem.type() == BSONType::string) && (elem.String() == kAsUserFragment)) {
            return ParsedPrivilegeFormat(PrivilegeFormat::kShowAsUserFragment);
        }
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Failed to parse 'showPrivileges'. 'showPrivileges' should "
                                   "either be a boolean or the string 'asUserFragment', given: "
                                << elem.toString());
    }

    void serializeToBSON(BSONArrayBuilder* bab) const {
        if (_format == PrivilegeFormat::kShowAsUserFragment) {
            bab->append(kAsUserFragment);
        } else {
            bab->append(_format == PrivilegeFormat::kShowSeparate);
        }
    }

    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* bob) const {
        if (_format == PrivilegeFormat::kShowAsUserFragment) {
            bob->append(fieldName, kAsUserFragment);
        } else {
            bob->append(fieldName, _format == PrivilegeFormat::kShowSeparate);
        }
    }

private:
    PrivilegeFormat _format;
};

}  // namespace auth
}  // namespace mongo
