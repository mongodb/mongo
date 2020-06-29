/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <string>
#include <utility>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/time_support.h"

namespace mongo {

using UserFieldname = std::string;
// These are the non-compound types from bsonspec.org.
using UserDouble = double;
using UserString = std::string;
using UserBinary = BSONBinData;
struct UserUndefined {};
using UserObjectId = OID;
using UserBoolean = bool;
using UserDate = Date_t;
struct UserNull {};
using UserRegex = BSONRegEx;
using UserDBPointer = BSONDBRef;
using UserJavascript = BSONCode;
using UserSymbol = BSONSymbol;
using UserJavascriptWithScope = BSONCodeWScope;
using UserInt = int;
using UserTimestamp = Timestamp;
using UserLong = long long;
using UserDecimal = Decimal128;
struct UserMinKey {};
struct UserMaxKey {};

struct CNode {
    static auto noopLeaf() {
        return CNode{Children{}};
    }

    auto toString() const {
        return toStringHelper(0);
    }

private:
    std::string toStringHelper(int numTabs) const;

public:
    using Fieldname = stdx::variant<KeyFieldname, UserFieldname>;
    using Children = std::vector<std::pair<Fieldname, CNode>>;
    stdx::variant<Children,
                  KeyValue,
                  UserDouble,
                  UserString,
                  UserBinary,
                  UserUndefined,
                  UserObjectId,
                  UserBoolean,
                  UserDate,
                  UserNull,
                  UserRegex,
                  UserDBPointer,
                  UserJavascript,
                  UserSymbol,
                  UserJavascriptWithScope,
                  UserInt,
                  UserTimestamp,
                  UserLong,
                  UserDecimal,
                  UserMinKey,
                  UserMaxKey>
        payload;
};

}  // namespace mongo
