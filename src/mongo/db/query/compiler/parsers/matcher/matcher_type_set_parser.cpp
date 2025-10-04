/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/parsers/matcher/matcher_type_set_parser.h"

#include "mongo/base/status.h"
#include "mongo/db/query/compiler/parsers/matcher/schema/json_schema_parser.h"
#include "mongo/util/str.h"

namespace mongo::parsers::matcher {
namespace {
/**
 * Parses an element containing either a numerical type code or a string type alias and adds the
 * resulting type to 'typeSet'. The 'aliasMapFind' function is used to map strings to BSON types.
 *
 * Returns a non-OK status if 'elt' does not represent a valid type.
 */
Status parseSingleType(BSONElement elt,
                       const findBSONTypeAliasFun& aliasMapFind,
                       MatcherTypeSet* typeSet) {
    if (!elt.isNumber() && elt.type() != BSONType::string) {
        return Status(ErrorCodes::TypeMismatch, "type must be represented as a number or a string");
    }

    if (elt.type() == BSONType::string) {
        return addAliasToTypeSet(elt.valueStringData(), aliasMapFind, typeSet);
    }

    auto valueAsInt = elt.parseIntegerElementToInt();
    if (!valueAsInt.isOK()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid numerical type code: " << elt.number());
    }

    if (valueAsInt.getValue() == 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid numerical type code: " << elt.number()
                                    << ". Instead use {$exists:false}.");
    }

    if (!isValidBSONType(valueAsInt.getValue())) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid numerical type code: " << elt.number());
    }

    typeSet->bsonTypes.insert(static_cast<BSONType>(valueAsInt.getValue()));
    return Status::OK();
}
}  // namespace

StatusWith<MatcherTypeSet> parseMatcherTypeSet(BSONElement elt) {
    MatcherTypeSet typeSet;

    if (elt.type() != BSONType::array) {
        auto status = parseSingleType(elt, findBSONTypeAlias, &typeSet);
        if (!status.isOK()) {
            return status;
        }
        return typeSet;
    }

    for (auto&& typeArrayElt : elt.embeddedObject()) {
        auto status = parseSingleType(typeArrayElt, findBSONTypeAlias, &typeSet);
        if (!status.isOK()) {
            return status;
        }
    }

    return typeSet;
}

}  // namespace mongo::parsers::matcher
