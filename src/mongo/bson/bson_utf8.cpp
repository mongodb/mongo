/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bson_utf8.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/str_escape.h"

#include <algorithm>

namespace mongo {
bool isValidUTF8(const BSONObj& obj) {
    return std::all_of(obj.begin(), obj.end(), [](const auto& elem) {
        if (!str::validUTF8(elem.fieldNameStringData())) {
            return false;
        }
        switch (elem.type()) {
            // Base cases.
            case BSONType::code:
            case BSONType::string:
            case BSONType::symbol: {
                return str::validUTF8(elem.valueStringData());
            }
            case BSONType::dbRef: {
                return str::validUTF8(elem.dbrefNS());
            }
            case BSONType::regEx: {
                return str::validUTF8(elem.regex()) && str::validUTF8(elem.regexFlags());
            }
            // Recursive cases.
            case BSONType::object: {
                return isValidUTF8(elem.Obj());
            }
            case BSONType::array: {
                return isValidUTF8(elem.Obj());
            }
            case BSONType::codeWScope: {
                return str::validUTF8(elem.codeWScopeCode()) &&
                    isValidUTF8(elem.codeWScopeObject());
            }
            default:
                return true;
        }
    });
}

void scrubInvalidUTF8(BSONElement elem, StringData fieldName, BSONObjBuilder& subObjBuilder) {
    auto scrub = [](StringData sd) {
        return str::validUTF8(sd) ? std::string{sd} : str::scrubInvalidUTF8(sd);
    };

    switch (elem.type()) {
        case BSONType::code: {
            subObjBuilder.appendCode(fieldName, scrub(elem.valueStringData()));
            break;
        }
        case BSONType::string: {
            subObjBuilder.append(fieldName, scrub(elem.valueStringData()));
            break;
        }
        case BSONType::symbol: {
            subObjBuilder.appendSymbol(fieldName, scrub(elem.valueStringData()));
            break;
        }
        case BSONType::dbRef: {
            subObjBuilder.appendDBRef(fieldName, scrub(elem.dbrefNS()), elem.dbrefOID());
            break;
        }
        case BSONType::regEx: {
            // Scrub the specific component(s) of the regex expression that has invalid UTF-8.
            auto regexStr = scrub(elem.regex());
            auto regexFlags = scrub(elem.regexFlags());
            subObjBuilder.appendRegex(fieldName, regexStr, regexFlags);
            break;
        }
        default:
            subObjBuilder.appendAs(elem, fieldName);
            break;
    }
}

void scrubInvalidUTF8(const BSONObj& obj, BSONObjBuilder& parentBuilder) {
    for (auto&& elem : obj) {
        auto fieldName = elem.fieldNameStringData();

        switch (elem.type()) {
            // Recursive cases.
            case BSONType::object: {
                BSONObjBuilder sub(parentBuilder.subobjStart(
                    str::validUTF8(fieldName) ? fieldName : str::scrubInvalidUTF8(fieldName)));
                scrubInvalidUTF8(elem.Obj(), sub);
                break;
            }
            case BSONType::array: {
                BSONObjBuilder sub(parentBuilder.subarrayStart(
                    str::validUTF8(fieldName) ? fieldName : str::scrubInvalidUTF8(fieldName)));
                scrubInvalidUTF8(elem.Obj(), sub);
                break;
            }
            case BSONType::codeWScope: {
                // CodeWScope has both a String and a BSONObj, so we want to recursively call
                // scrubInvalidUTF8 on the BSONObj and also scrub the String in the Code
                // portion.
                auto codeStr = elem.codeWScopeCode();
                auto scopeObj = elem.codeWScopeObject();

                // Scrub the BSONObj within the CodeWScope.
                BSONObjBuilder codeWScopeSubBuilder;
                scrubInvalidUTF8(scopeObj, codeWScopeSubBuilder);

                // Add the scrubbed object and the scrubbed CodeStr.
                parentBuilder.appendCodeWScope(
                    str::validUTF8(fieldName) ? fieldName : str::scrubInvalidUTF8(fieldName),
                    str::validUTF8(codeStr) ? codeStr : str::scrubInvalidUTF8(codeStr),
                    codeWScopeSubBuilder.obj());
                break;
            }
            // Base cases.
            default: {
                auto fieldName = elem.fieldNameStringData();
                if (str::validUTF8(fieldName)) {
                    scrubInvalidUTF8(elem, fieldName, parentBuilder);
                } else {
                    scrubInvalidUTF8(elem, str::scrubInvalidUTF8(fieldName), parentBuilder);
                }
                break;
            }
        }
    }
}

BSONObj checkAndScrubInvalidUTF8(BSONObj obj) {
    if (isValidUTF8(obj)) {
        // Case where we didn't need to scrub anything.
        return obj;
    }

    // Create parent BSONObjBuilder where we will store our scrubbed BSONObj.
    BSONObjBuilder builder;
    scrubInvalidUTF8(obj, builder);
    return builder.obj();
}
}  // namespace mongo
