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

#pragma once

#include <string>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {

/**
 * Represents a type alias in the match language. This is either a particular BSON type, or the
 * "number" type, which is is the union of all numeric BSON types.
 */
struct MatcherTypeAlias {
    static constexpr StringData kMatchesAllNumbersAlias = "number"_sd;
    static const stdx::unordered_map<std::string, BSONType> typeAliasMap;

    /**
     * Creates a MatcherTypeAlias from a BSONElement. Returns an error if the element does not
     * contain a valid numerical type code or a valid string type alias.
     */
    static StatusWith<MatcherTypeAlias> parse(BSONElement);

    /**
     * Creates a MatcherTypeAlias from a type alias string, or returns an error if the alias is not
     * valid.
     */
    static StatusWith<MatcherTypeAlias> parseFromStringAlias(StringData typeAlias);

    MatcherTypeAlias() = default;

    /* implicit */ MatcherTypeAlias(BSONType bsonType) : bsonType(bsonType) {}

    /**
     * Returns whether the element is of a type which matches this type.
     */
    bool elementMatchesType(BSONElement) const;

    bool allNumbers = false;
    BSONType bsonType = BSONType::EOO;
};

}  // namespace mongo
