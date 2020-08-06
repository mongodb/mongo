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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/update/delta_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(DeltaExecutorTest, Delete) {
    BSONObj preImage(fromjson("{f1: {a: {b: {c: 1}, c: 1}}}"));
    UpdateIndexData indexData;
    indexData.addPath(FieldRef("p.a.b"));
    indexData.addPath(FieldRef("f1.a.b"));
    FieldRefSet fieldRefSet;
    {
        // When a path in the diff is a prefix of index path.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        DeltaExecutor test(fromjson("{d: {f1: false, f2: false, f3: false}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(), BSONObj());
        ASSERT(result.indexesAffected);
    }

    {
        // When a path in the diff is same as index path.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        DeltaExecutor test(fromjson("{sf1: {sa: {d: {p: false, c: false, b: false}}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When the index path is a prefix of a path in the diff.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {sb: {d: {c: false}}}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {}, c: 1}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // With common parent, but path diverges.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {d: {c: false}}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {c: 1}}}}"));
        ASSERT(!result.indexesAffected);
    }
}

TEST(DeltaExecutorTest, Update) {
    BSONObj preImage(fromjson("{f1: {a: {b: {c: 1}, c: 1}}}"));
    UpdateIndexData indexData;
    indexData.addPath(FieldRef("p.a.b"));
    indexData.addPath(FieldRef("f1.a.b"));
    FieldRefSet fieldRefSet;
    {
        // When a path in the diff is a prefix of index path.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        DeltaExecutor test(fromjson("{u: {f1: false, f2: false, f3: false}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: false, f2: false, f3: false}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When a path in the diff is same as index path.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {u: {p: false, c: false, b: false}}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: false, c: false, p: false}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When the index path is a prefix of a path in the diff.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {sb: {u: {c: false}}}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {c: false}, c: 1}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // With common parent, but path diverges.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {u: {c: false}}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {c: 1}, c: false}}}"));
        ASSERT(!result.indexesAffected);
    }
}

TEST(DeltaExecutorTest, Insert) {
    UpdateIndexData indexData;
    indexData.addPath(FieldRef("p.a.b"));
    // 'UpdateIndexData' will canonicalize the path and remove all numeric components. So the '2'
    // and '33' components should not matter.
    indexData.addPath(FieldRef("f1.2.a.33.b"));
    FieldRefSet fieldRefSet;
    {
        // When a path in the diff is a prefix of index path.
        auto doc = mutablebson::Document(BSONObj());
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        DeltaExecutor test(fromjson("{i: {f1: false, f2: false, f3: false}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: false, f2: false, f3: false}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When a path in the diff is same as index path.
        auto doc = mutablebson::Document(fromjson("{f1: {a: {c: true}}}}"));
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {i: {p: false, c: false, b: false}}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {p: false, c: false, b: false}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When the index path is a prefix of a path in the diff.
        auto doc = mutablebson::Document(fromjson("{f1: {a: {b: {c: {e: 1}}}}}"));
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {sb: {sc: {i : {d: 2} }}}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {c: {e: 1, d: 2}}}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // With common parent, but path diverges.
        auto doc = mutablebson::Document(fromjson("{f1: {a: {b: {c: 1}}}}"));
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {i: {c: 2}}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {c: 1}, c: 2}}}"));
        ASSERT(!result.indexesAffected);
    }
}

TEST(DeltaExecutorTest, ArraysInIndexPath) {
    BSONObj preImage(fromjson("{f1: [{a: {b: {c: 1}, c: 1}}, 1]}"));
    UpdateIndexData indexData;
    indexData.addPath(FieldRef("p.a.b"));
    // Numeric components will be ignored, so they should not matter.
    indexData.addPath(FieldRef("f1.9.a.10.b"));
    FieldRefSet fieldRefSet;
    {
        // Test resize.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        DeltaExecutor test(fromjson("{sf1: {a: true, l: 1}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: [{a: {b: {c: 1}, c: 1}}]}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When the index path is a prefix of a path in the diff and also involves numeric
        // components along the way. The numeric components should always be ignored.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {a: true, s0: {sa: {sb: {i: {d: 1} }}}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: [{a: {b: {c: 1, d: 1}, c: 1}}, 1]}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When inserting a sub-object into array, and the sub-object diverges from the index path.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {a: true, u2: {b: 1}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: [{a: {b: {c: 1}, c: 1}}, 1, {b:1}]}"));
        ASSERT(result.indexesAffected);
    }
    {
        // When a common array path element is updated, but the paths diverge at the last element.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {a: true, s0: {sa: {d: {c: false} }}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: [{a: {b: {c: 1}}}, 1]}"));
        ASSERT(!result.indexesAffected);
    }
}

TEST(DeltaExecutorTest, ArraysAfterIndexPath) {
    BSONObj preImage(fromjson("{f1: {a: {b: [{c: 1}, 2]}}}"));
    UpdateIndexData indexData;
    indexData.addPath(FieldRef("p.a.b"));
    // 'UpdateIndexData' will canonicalize the path and remove all numeric components. So the '9'
    // and '10' components should not matter.
    indexData.addPath(FieldRef("f1.9.a.10"));
    FieldRefSet fieldRefSet;
    {
        // Test resize.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        DeltaExecutor test(fromjson("{sf1: {sa: {sb: {a: true, l: 1}}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: [{c: 1}]}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // Updating a sub-array element.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {sb: {a: true, s0: {u: {c: 2}} }}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: [{c: 2}, 2]}}}"));
        ASSERT(result.indexesAffected);
    }
    {
        // Updating an array element.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        params.indexData = &indexData;
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {sb: {a: true, u0: 1 }}}}"));
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: [1, 2]}}}"));
        ASSERT(result.indexesAffected);
    }
}

}  // namespace
}  // namespace mongo
