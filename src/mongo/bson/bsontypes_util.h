// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/util/modules.h"

#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

// Utility class to allow adding a std::string to BSON as a Symbol
struct BSONSymbol {
    BSONSymbol() = default;
    explicit BSONSymbol(std::string_view sym) : symbol(sym) {}
    std::string_view symbol = "";
};

// Utility class to allow adding a std::string to BSON as Code
struct BSONCode {
    BSONCode() = default;
    explicit BSONCode(std::string_view str) : code(str) {}
    std::string_view code = "";
};

// Utility class to allow adding CodeWScope to BSON
struct BSONCodeWScope {
    BSONCodeWScope() = default;
    explicit BSONCodeWScope(std::string_view str, const BSONObj& obj) : code(str), scope(obj) {}
    std::string_view code = "";
    BSONObj scope = {};
};

// Utility class to allow adding a RegEx to BSON
struct BSONRegEx {
    explicit BSONRegEx(std::string_view pat = "", std::string_view f = "")
        : pattern(pat), flags(f) {}
    std::string_view pattern;
    std::string_view flags;
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
    BSONDBRef(std::string_view nameSpace, const OID& o) : ns(nameSpace), oid(o) {}
    std::string_view ns = "";
    OID oid;
};


}  // namespace mongo
