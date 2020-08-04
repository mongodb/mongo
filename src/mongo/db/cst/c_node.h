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

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/time_support.h"

namespace mongo {

using UserFieldname = std::string;
using NonZeroKey = stdx::variant<int, long long, double, Decimal128>;
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
        return CNode{ObjectChildren{}};
    }

    /*
     * Produce the payload of this CNode representing a string. Throws a fatal exception if this
     * CNode does not represent a string.
     */
    auto& getString() const {
        return stdx::get<UserString>(payload);
    }

    /*
     * Produce the payload of this CNode representing a boolean. Throws a fatal exception if this
     * CNode does not represent a boolean.
     */
    auto& getBool() const {
        return stdx::get<UserBoolean>(payload);
    }

    /**
     * Produce a string formatted with tabs and endlines that describes the CST underneath this
     * CNode.
     */
    auto toString() const {
        return toStringHelper(0) + "\n";
    }

    /**
     * Produce BSON representing this CST. This is for debugging and testing with structured output,
     * not for decompiling to the input query. The produced BSON will consist of arrays, objects,
     * and descriptive strings only. This version also returns bool that indicates if the returned
     * BSON is a BSONArray.
     */
    std::pair<BSONObj, bool> toBsonWithArrayIndicator() const;
    /**
     * Produce BSON representing this CST. This is for debugging and testing with structured output,
     * not for decompiling to the input query. The produced BSON will consist of arrays, objects,
     * and descriptive strings only.
     */
    BSONObj toBson() const {
        return toBsonWithArrayIndicator().first;
    }

    /*
     * Produce the children of this CNode representing an array. Throws a fatal exception if this
     * CNode does not represent an array. Const version.
     */
    auto& arrayChildren() const {
        return stdx::get<ArrayChildren>(payload);
    }
    /*
     * Produce the children of this CNode representing an array. Throws a fatal exception if this
     * CNode does not represent an array. Non-const version.
     */
    auto& arrayChildren() {
        return stdx::get<ArrayChildren>(payload);
    }

    /*
     * Produce the children of this CNode representing an object. Throws a fatal exception if this
     * CNode does not represent an object. Const version.
     */
    auto& objectChildren() const {
        return stdx::get<ObjectChildren>(payload);
    }
    /*
     * Produce the children of this CNode representing an object. Throws a fatal exception if this
     * CNode does not represent an object. Non-const version.
     */
    auto& objectChildren() {
        return stdx::get<ObjectChildren>(payload);
    }

    /*
     * Produce the KeyFieldname of the first element of this CNode representing an object. Throws a
     * fatal exception if this CNode does not represent an object, if it is an empty object or if
     * the first element does not have a KeyFieldname. Const version.
     */
    auto& firstKeyFieldname() const {
        dassert(objectChildren().size() > 0);
        return stdx::get<KeyFieldname>(objectChildren().begin()->first);
    }
    /*
     * Produce the KeyFieldname of the first element of this CNode representing an object. Throws a
     * fatal exception if this CNode does not represent an object, if it is an empty object or if
     * the first element does not have a KeyFieldname. Non-const version.
     */
    auto& firstKeyFieldname() {
        dassert(objectChildren().size() > 0);
        return stdx::get<KeyFieldname>(objectChildren().begin()->first);
    }

private:
    std::string toStringHelper(int numTabs) const;

public:
    using Fieldname = stdx::variant<KeyFieldname, UserFieldname>;
    using ArrayChildren = std::vector<CNode>;
    using ObjectChildren = std::vector<std::pair<Fieldname, CNode>>;
    stdx::variant<ArrayChildren,
                  ObjectChildren,
                  KeyValue,
                  NonZeroKey,
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
