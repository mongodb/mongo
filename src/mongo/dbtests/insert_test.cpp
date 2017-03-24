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

#include "mongo/bson/bson_depth.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/ops/insert.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
const auto kInsertTestNss = NamespaceString{"dbtests.InsertTest"};

class InsertTest : public unittest::Test {
public:
    InsertTest()
        : _opCtx(cc().makeOperationContext()),
          _scopedTransaction(_opCtx.get(), MODE_IX),
          _lock(_opCtx.get()->lockState()),
          _autoColl(_opCtx.get(), kInsertTestNss, MODE_IX) {}

    const OperationContext* getOperationContext() const {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    ScopedTransaction _scopedTransaction;
    Lock::GlobalWrite _lock;
    AutoGetCollection _autoColl;
};

BSONObj makeNestedObject(size_t depth) {
    ASSERT_GTE(depth, 1U);

    auto obj = BSON("a" << 1);
    while (--depth) {
        obj = BSON("a" << obj);
    }

    return obj;
}

BSONObj makeNestedArray(size_t depth) {
    ASSERT_GTE(depth, 1U);

    auto obj = BSON_ARRAY(1);
    while (--depth) {
        obj = BSON_ARRAY(obj);
    }

    return obj;
}

TEST_F(InsertTest, FixDocumentForInsertAcceptsEmptyDocuments) {
    ASSERT_OK(fixDocumentForInsert(BSONObj()));
}

TEST_F(InsertTest, FixDocumentForInsertAcceptsDocumentsAtStorageDepthLimit) {
    ASSERT_OK(fixDocumentForInsert(makeNestedObject(BSONDepth::getMaxDepthForUserStorage())));
    ASSERT_OK(fixDocumentForInsert(makeNestedArray(BSONDepth::getMaxDepthForUserStorage())));
}

TEST_F(InsertTest, FixDocumentForInsertFailsOnDeeplyNestedDocuments) {
    ASSERT_EQ(fixDocumentForInsert(makeNestedObject(BSONDepth::getMaxDepthForUserStorage() + 1)),
              ErrorCodes::Overflow);
    ASSERT_EQ(fixDocumentForInsert(makeNestedArray(BSONDepth::getMaxDepthForUserStorage() + 1)),
              ErrorCodes::Overflow);
}
}  // namespace
}  // namespace mongo
