/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"

namespace mongo {

// Utility class to allow adding a std::string to BSON as a Symbol
struct BSONSymbol {
    BSONSymbol() = default;
    explicit BSONSymbol(StringData sym) : symbol(sym) {}
    StringData symbol = "";
};

// Utility class to allow adding a std::string to BSON as Code
struct BSONCode {
    BSONCode() = default;
    explicit BSONCode(StringData str) : code(str) {}
    StringData code = "";
};

// Utility class to allow adding CodeWScope to BSON
struct BSONCodeWScope {
    BSONCodeWScope() = default;
    explicit BSONCodeWScope(StringData str, const BSONObj& obj) : code(str), scope(obj) {}
    StringData code = "";
    BSONObj scope = {};
};

// Utility class to allow adding a RegEx to BSON
struct BSONRegEx {
    explicit BSONRegEx(StringData pat = "", StringData f = "") : pattern(pat), flags(f) {}
    StringData pattern;
    StringData flags;
};

// Utility class to allow adding binary data to BSON
struct BSONBinData {
    BSONBinData() = default;
    BSONBinData(const void* d, int l, BinDataType t) : data(d), length(l), type(t) {}
    const void* data = nullptr;
    int length = 0;
    BinDataType type = BinDataGeneral;
};

// Utility class to allow adding deprecated DBRef type to BSON
struct BSONDBRef {
    BSONDBRef() = default;
    BSONDBRef(StringData nameSpace, const OID& o) : ns(nameSpace), oid(o) {}
    StringData ns = "";
    OID oid;
};


}  // namespace mongo
