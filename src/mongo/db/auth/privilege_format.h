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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

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

/**
 * Proxy for PrivilegeFormat to parse into and out of IDL formats.
 */
class ParsedPrivilegeFormat {
public:
    static constexpr StringData kAsUserFragment = "asUserFragment"_sd;

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

    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const {
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
