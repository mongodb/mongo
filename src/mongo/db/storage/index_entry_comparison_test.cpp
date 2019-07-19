/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(IndexEntryComparison, BuildDupKeyErrorStatusProducesExpectedErrorObject) {
    NamespaceString collNss("test.foo");
    std::string indexName("a_1_b_1");
    auto keyPattern = BSON("a" << 1 << "b" << 1);
    auto keyValue = BSON("" << 10 << ""
                            << "abc");

    auto dupKeyStatus = buildDupKeyErrorStatus(keyValue, collNss, indexName, keyPattern);
    ASSERT_NOT_OK(dupKeyStatus);
    ASSERT_EQUALS(dupKeyStatus.code(), ErrorCodes::DuplicateKey);

    auto extraInfo = dupKeyStatus.extraInfo<DuplicateKeyErrorInfo>();
    ASSERT(extraInfo);

    ASSERT_BSONOBJ_EQ(extraInfo->getKeyPattern(), keyPattern);

    auto keyValueWithFieldName = BSON("a" << 10 << "b"
                                          << "abc");
    ASSERT_BSONOBJ_EQ(extraInfo->getDuplicatedKeyValue(), keyValueWithFieldName);

    BSONObjBuilder objBuilder;
    extraInfo->serialize(&objBuilder);
    ASSERT_BSONOBJ_EQ(objBuilder.obj(),
                      BSON("keyPattern" << keyPattern << "keyValue" << keyValueWithFieldName));
}

}  // namespace mongo
