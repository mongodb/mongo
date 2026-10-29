// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <forward_list>
#include <string_view>

namespace mongo::bsoncolumn {

BSONElement createElementDouble(double val, std::forward_list<BSONObj>& elementMemory);

BSONElement createObjectId(OID val, std::forward_list<BSONObj>& elementMemory);

BSONElement createTimestamp(Timestamp val, std::forward_list<BSONObj>& elementMemory);

BSONElement createElementInt64(int64_t val, std::forward_list<BSONObj>& elementMemory);

BSONElement createElementInt32(int32_t val, std::forward_list<BSONObj>& elementMemory);

BSONElement createElementDecimal128(Decimal128 val, std::forward_list<BSONObj>& elementMemory);

BSONElement createDate(Date_t dt, std::forward_list<BSONObj>& elementMemory);

BSONElement createBool(bool b, std::forward_list<BSONObj>& elementMemory);

BSONElement createElementMinKey(std::forward_list<BSONObj>& elementMemory);

BSONElement createElementMaxKey(std::forward_list<BSONObj>& elementMemory);

BSONElement createNull(std::forward_list<BSONObj>& elementMemory);

BSONElement createUndefined(std::forward_list<BSONObj>& elementMemory);

BSONElement createRegex(std::string_view pattern,
                        std::string_view options,
                        std::forward_list<BSONObj>& elementMemory);

BSONElement createDBRef(std::string_view ns,
                        const OID& oid,
                        std::forward_list<BSONObj>& elementMemory);

BSONElement createElementCode(std::string_view code, std::forward_list<BSONObj>& elementMemory);

BSONElement createCodeWScope(std::string_view code,
                             const BSONObj& scope,
                             std::forward_list<BSONObj>& elementMemory);

BSONElement createSymbol(std::string_view symbol, std::forward_list<BSONObj>& elementMemory);

BSONElement createElementBinData(BinDataType binDataType,
                                 const char* buf,
                                 size_t len,
                                 std::forward_list<BSONObj>& elementMemory);

BSONElement createElementString(std::string_view val, std::forward_list<BSONObj>& elementMemory);

BSONElement createElementObj(BSONObj obj, std::forward_list<BSONObj>& elementMemory);

BSONElement createElementArray(BSONArray arr, std::forward_list<BSONObj>& elementMemory);

bool createFuzzedObj(const char*& ptr,
                     const char* end,
                     std::forward_list<BSONObj>& elementMemory,
                     BSONObj& result,
                     size_t depth);

bool createFuzzedElement(const char*& ptr,
                         const char* end,
                         std::forward_list<BSONObj>& elementMemory,
                         int& repetition,
                         BSONElement& result,
                         size_t depth = 0);

bool addFuzzedElements(const char*& ptr,
                       const char* end,
                       std::forward_list<BSONObj>& elementMemory,
                       BSONElement element,
                       int repetition,
                       std::vector<BSONElement>& generatedElements);

}  // namespace mongo::bsoncolumn
