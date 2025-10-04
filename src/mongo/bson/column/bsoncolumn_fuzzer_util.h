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
#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"

#include <forward_list>

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

BSONElement createRegex(StringData pattern,
                        StringData options,
                        std::forward_list<BSONObj>& elementMemory);

BSONElement createDBRef(StringData ns, const OID& oid, std::forward_list<BSONObj>& elementMemory);

BSONElement createElementCode(StringData code, std::forward_list<BSONObj>& elementMemory);

BSONElement createCodeWScope(StringData code,
                             const BSONObj& scope,
                             std::forward_list<BSONObj>& elementMemory);

BSONElement createSymbol(StringData symbol, std::forward_list<BSONObj>& elementMemory);

BSONElement createElementBinData(BinDataType binDataType,
                                 const char* buf,
                                 size_t len,
                                 std::forward_list<BSONObj>& elementMemory);

BSONElement createElementString(StringData val, std::forward_list<BSONObj>& elementMemory);

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
