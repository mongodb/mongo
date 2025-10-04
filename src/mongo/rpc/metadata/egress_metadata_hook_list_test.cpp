/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/rpc/metadata/egress_metadata_hook_list.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>

using std::string;

namespace mongo {
namespace rpc {
namespace {

struct ReadReplyArgs {
public:
    BSONObj metadataObj;
};

class TestHook : public EgressMetadataHook {
public:
    TestHook(string fieldName, ReadReplyArgs* arg) : _fieldName(fieldName), _arg(arg) {}

    Status writeRequestMetadata(OperationContext* opCtx, BSONObjBuilder* metadataBob) override {
        metadataBob->append(_fieldName, "");
        return Status::OK();
    }

    Status readReplyMetadata(OperationContext* opCtx, const BSONObj& metadataObj) override {
        invariant(_arg != nullptr);
        _arg->metadataObj = metadataObj;
        return Status::OK();
    }

private:
    const string _fieldName;
    ReadReplyArgs* _arg;
};

class FixedStatusTestHook : public EgressMetadataHook {
public:
    FixedStatusTestHook(Status status) : _toRet(status) {}

    Status writeRequestMetadata(OperationContext* opCtx, BSONObjBuilder* metadataBob) override {
        return _toRet;
    }

    Status readReplyMetadata(OperationContext* opCtx, const BSONObj& metadataObj) override {
        return _toRet;
    }

private:
    Status _toRet;
};

TEST(EgressMetadataHookListTest, EmptyHookShouldNotFail) {
    EgressMetadataHookList hookList;
    ASSERT_OK(hookList.writeRequestMetadata(nullptr, nullptr));

    BSONObj emptyObj;
    ASSERT_OK(hookList.readReplyMetadata(nullptr, emptyObj));
}

TEST(EgressMetadataHookListTest, SingleHook) {
    ReadReplyArgs hook1Args;
    auto hook1 = std::make_unique<TestHook>("h1", &hook1Args);
    EgressMetadataHookList hookList;
    hookList.addHook(std::move(hook1));

    BSONObjBuilder builder;
    ASSERT_OK(hookList.writeRequestMetadata(nullptr, &builder));
    ASSERT_BSONOBJ_EQ(BSON("h1" << ""), builder.obj());

    BSONObj testObj(BSON("x" << 1));
    ASSERT_OK(hookList.readReplyMetadata(nullptr, testObj));
    ASSERT_BSONOBJ_EQ(testObj, hook1Args.metadataObj);
}

TEST(EgressMetadataHookListTest, MultipleHooks) {
    ReadReplyArgs hook1Args;
    auto hook1 = std::make_unique<TestHook>("foo", &hook1Args);
    EgressMetadataHookList hookList;
    hookList.addHook(std::move(hook1));

    ReadReplyArgs hook2Args;
    auto hook2 = std::make_unique<TestHook>("bar", &hook2Args);
    hookList.addHook(std::move(hook2));

    BSONObjBuilder builder;
    ASSERT_OK(hookList.writeRequestMetadata(nullptr, &builder));
    ASSERT_BSONOBJ_EQ(BSON("foo" << ""
                                 << "bar"
                                 << ""),
                      builder.obj());

    BSONObj testObj(BSON("x" << 1));
    ASSERT_OK(hookList.readReplyMetadata(nullptr, testObj));

    ASSERT_BSONOBJ_EQ(testObj, hook1Args.metadataObj);

    ASSERT_BSONOBJ_EQ(testObj, hook2Args.metadataObj);
}

TEST(EgressMetadataHookListTest, SingleBadHookShouldReturnError) {
    ReadReplyArgs hook1Args;
    auto hook1 = std::make_unique<TestHook>("foo", &hook1Args);
    EgressMetadataHookList hookList;
    hookList.addHook(std::move(hook1));

    Status err{ErrorCodes::IllegalOperation, "intentional error by test"};
    auto hook2 = std::make_unique<FixedStatusTestHook>(err);
    hookList.addHook(std::move(hook2));

    BSONObjBuilder builder;
    ASSERT_NOT_OK(hookList.writeRequestMetadata(nullptr, &builder));
    ASSERT_NOT_OK(hookList.readReplyMetadata(nullptr, BSON("x" << 1)));
}

}  // unnamed namespace
}  // namespace rpc
}  // namespace mongo
