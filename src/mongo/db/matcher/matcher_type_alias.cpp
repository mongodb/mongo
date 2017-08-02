/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/matcher_type_alias.h"

namespace mongo {

constexpr StringData MatcherTypeAlias::kMatchesAllNumbersAlias;

const stdx::unordered_map<std::string, BSONType> MatcherTypeAlias::typeAliasMap = {
    {typeName(BSONType::NumberDouble), BSONType::NumberDouble},
    {typeName(BSONType::String), BSONType::String},
    {typeName(BSONType::Object), BSONType::Object},
    {typeName(BSONType::Array), BSONType::Array},
    {typeName(BSONType::BinData), BSONType::BinData},
    {typeName(BSONType::Undefined), BSONType::Undefined},
    {typeName(BSONType::jstOID), BSONType::jstOID},
    {typeName(BSONType::Bool), BSONType::Bool},
    {typeName(BSONType::Date), BSONType::Date},
    {typeName(BSONType::jstNULL), BSONType::jstNULL},
    {typeName(BSONType::RegEx), BSONType::RegEx},
    {typeName(BSONType::DBRef), BSONType::DBRef},
    {typeName(BSONType::Code), BSONType::Code},
    {typeName(BSONType::Symbol), BSONType::Symbol},
    {typeName(BSONType::CodeWScope), BSONType::CodeWScope},
    {typeName(BSONType::NumberInt), BSONType::NumberInt},
    {typeName(BSONType::bsonTimestamp), BSONType::bsonTimestamp},
    {typeName(BSONType::NumberLong), BSONType::NumberLong},
    {typeName(BSONType::NumberDecimal), BSONType::NumberDecimal},
    {typeName(BSONType::MaxKey), BSONType::MaxKey},
    {typeName(BSONType::MinKey), BSONType::MinKey}};

StatusWith<MatcherTypeAlias> MatcherTypeAlias::parseFromStringAlias(StringData typeAlias) {
    if (typeAlias == MatcherTypeAlias::kMatchesAllNumbersAlias) {
        MatcherTypeAlias type;
        type.allNumbers = true;
        return type;
    }

    auto it = MatcherTypeAlias::typeAliasMap.find(typeAlias.toString());
    if (it == MatcherTypeAlias::typeAliasMap.end()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Unknown type name alias: " << typeAlias);
    }

    return {it->second};
}

StatusWith<MatcherTypeAlias> MatcherTypeAlias::parse(BSONElement elt) {
    if (!elt.isNumber() && elt.type() != BSONType::String) {
        return Status(ErrorCodes::TypeMismatch, "type must be represented as a number or a string");
    }

    if (elt.type() == BSONType::String) {
        return parseFromStringAlias(elt.valueStringData());
    }

    invariant(elt.isNumber());
    int typeInt = elt.numberInt();
    if (elt.type() != BSONType::NumberInt && typeInt != elt.number()) {
        typeInt = -1;
    }

    if (!isValidBSONType(typeInt)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid numerical type code: " << typeInt);
    }

    return {static_cast<BSONType>(typeInt)};
}

bool MatcherTypeAlias::elementMatchesType(BSONElement elt) const {
    if (allNumbers) {
        return elt.isNumber();
    }

    return elt.type() == bsonType;
}

}  // namespace mongo
