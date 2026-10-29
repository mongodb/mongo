// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/bson_utf8.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/str_escape.h"

#include <algorithm>
#include <string_view>

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

void scrubInvalidUTF8(BSONElement elem, std::string_view fieldName, BSONObjBuilder& subObjBuilder) {
    auto scrub = [](std::string_view sd) {
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
