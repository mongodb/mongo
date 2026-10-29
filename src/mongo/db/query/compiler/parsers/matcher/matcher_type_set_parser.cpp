// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
