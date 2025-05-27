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

#include "mongo/db/update/delta_executor.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update_index_data.h"
#include "mongo/unittest/unittest.h"

#include <vector>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

/**
 * Return a bitmask where each bit indicates whether the matching position in the indexData
 * argument is for an index that has been affected by the modification described by the logEntry
 * argument. E.g., a return value of 0 means "no indexes are affected", a return value of 1 (2^0)
 * means that the index whose UpdateIndexData descriptor is placed in the first position of the
 * indexData argument is affected by the changes, a return value of 2 (2^1) points to the second
 * entry in the indexData vector, a return value of 3 (1+2) indicates that both first and second
 * entry in the idnexDaa vector are affected by the modification.
 */
unsigned long getIndexAffectedFromLogEntry(std::vector<const UpdateIndexData*> indexData,
                                           BSONObj logEntry) {
    auto diff = update_oplog_entry::extractDiffFromOplogEntry(logEntry);
    if (!diff) {
        return (unsigned long)-1;
    }
    mongo::doc_diff::IndexUpdateIdentifier updateIdentifier{indexData.size() /*numIndexes*/};
    for (size_t i = 0; i < indexData.size(); i++) {
        updateIdentifier.addIndex(i /*indexCounter*/, *indexData[i]);
    }

    // Convert the set bits into a numeric value to return.
    // This assumes that the value fits into an unsigned long, which is true for the test cases in
    // this file.
    doc_diff::IndexSet indexSet = updateIdentifier.determineAffectedIndexes(*diff);
    unsigned long value = 0;
    for (size_t pos = indexSet.findFirst(); pos != mongo::doc_diff::IndexSet::npos;
         pos = indexSet.findNext(pos)) {
        value |= 1 << pos;
    }
    return value;
}

TEST(DeltaExecutorTest, Delete) {
    BSONObj preImage(fromjson("{f1: {a: {b: {c: 1}, c: 1}}}"));
    UpdateIndexData indexData1, indexData2;
    constexpr bool mustCheckExistenceForInsertOperations = true;
    indexData1.addPath(FieldRef("p.a.b"));
    indexData2.addPath(FieldRef("f1.a.b"));
    FieldRefSet fieldRefSet;
    {
        // When a path in the diff is a prefix of index path.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        DeltaExecutor test(fromjson("{d: {f1: false, f2: false, f3: false}}"),
                           mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(), BSONObj());
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }

    {
        // When a path in the diff is same as index path.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        DeltaExecutor test(fromjson("{sf1: {sa: {d: {p: false, c: false, b: false}}}}"),
                           mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {}}}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // When the index path is a prefix of a path in the diff.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {sb: {d: {c: false}}}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {}, c: 1}}}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // With common parent, but path diverges.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {d: {c: false}}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {c: 1}}}}"));
        ASSERT_EQ(0, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
}

TEST(DeltaExecutorTest, Update) {
    BSONObj preImage(fromjson("{f1: {a: {b: {c: 1}, c: 1}}}"));
    UpdateIndexData indexData1, indexData2;
    constexpr bool mustCheckExistenceForInsertOperations = true;
    indexData1.addPath(FieldRef("p.a.b"));
    indexData2.addPath(FieldRef("f1.a.b"));
    FieldRefSet fieldRefSet;
    {
        // When a path in the diff is a prefix of index path.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        DeltaExecutor test(fromjson("{u: {f1: false, f2: false, f3: false}}"),
                           mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: false, f2: false, f3: false}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // When a path in the diff is same as index path.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {u: {p: false, c: false, b: false}}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: false, c: false, p: false}}}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // When the index path is a prefix of a path in the diff.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {sb: {u: {c: false}}}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {c: false}, c: 1}}}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // With common parent, but path diverges.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {u: {c: false}}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {c: 1}, c: false}}}"));
        ASSERT_EQ(0, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
}

TEST(DeltaExecutorTest, Insert) {
    UpdateIndexData indexData1, indexData2;
    constexpr bool mustCheckExistenceForInsertOperations = true;
    indexData1.addPath(FieldRef("p.a.b"));
    // 'UpdateIndexData' will canonicalize the path and remove all numeric components. So the '2'
    // and '33' components should not matter.
    indexData2.addPath(FieldRef("f1.2.a.33.b"));
    FieldRefSet fieldRefSet;
    {
        // When a path in the diff is a prefix of index path.
        auto doc = mutablebson::Document(BSONObj());
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        DeltaExecutor test(fromjson("{i: {f1: false, f2: false, f3: false}}"),
                           mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: false, f2: false, f3: false}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // When a path in the diff is same as index path.
        auto doc = mutablebson::Document(fromjson("{f1: {a: {c: true}}}"));
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {i: {p: false, c: false, b: false}}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {p: false, c: false, b: false}}}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // When the index path is a prefix of a path in the diff.
        auto doc = mutablebson::Document(fromjson("{f1: {a: {b: {c: {e: 1}}}}}"));
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {sb: {sc: {i : {d: 2} }}}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {c: {e: 1, d: 2}}}}}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // With common parent, but path diverges.
        auto doc = mutablebson::Document(fromjson("{f1: {a: {b: {c: 1}}}}"));
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {i: {c: 2}}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: {c: 1}, c: 2}}}"));
        ASSERT_EQ(0, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
}

TEST(DeltaExecutorTest, InsertNumericFieldNamesTopLevel) {
    BSONObj preImage;
    UpdateIndexData indexData;
    constexpr bool mustCheckExistenceForInsertOperations = true;
    indexData.addPath(FieldRef{"1"});
    FieldRefSet fieldRefSet;

    // Insert numeric fields at the top level, which means they are guaranteed to be object key
    // names (rather than possibly being indices of an array).
    {
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        DeltaExecutor test(fromjson("{i: {'0': false, '1': false, '2': false}}"),
                           mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{'0': false, '1': false, '2': false}"));
        ASSERT_EQ(1, getIndexAffectedFromLogEntry({&indexData}, result.oplogEntry));
    }
    {
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        DeltaExecutor test(fromjson("{i: {'0': false, '2': false}}"),
                           mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{'0': false, '2': false}"));
        ASSERT_EQ(0, getIndexAffectedFromLogEntry({&indexData}, result.oplogEntry));
    }
}

TEST(DeltaExecutorTest, InsertNumericFieldNamesNested) {
    BSONObj preImage{fromjson("{a: {}}")};
    UpdateIndexData indexData;
    constexpr bool mustCheckExistenceForInsertOperations = true;
    indexData.addPath(FieldRef{"a.1"});
    FieldRefSet fieldRefSet;

    // Insert numeric fields not at the top level, which means they may possibly be indices of an
    // array.
    {
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        DeltaExecutor test(fromjson("{sa: {i: {'0': false, '1': false, '2': false}}}"),
                           mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{a: {'0': false, '1': false, '2': false}}"));
        ASSERT_EQ(1, getIndexAffectedFromLogEntry({&indexData}, result.oplogEntry));
    }
    {
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        DeltaExecutor test(fromjson("{sa: {i: {'0': false, '2': false}}}"),
                           mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{a: {'0': false, '2': false}}"));
        ASSERT_EQ(1, getIndexAffectedFromLogEntry({&indexData}, result.oplogEntry));
    }
}

TEST(DeltaExecutorTest, ArraysInIndexPath) {
    BSONObj preImage(fromjson("{f1: [{a: {b: {c: 1}, c: 1}}, 1]}"));
    UpdateIndexData indexData1, indexData2;
    constexpr bool mustCheckExistenceForInsertOperations = true;
    indexData1.addPath(FieldRef("p.a.b"));
    // Numeric components will be ignored, so they should not matter.
    indexData2.addPath(FieldRef("f1.9.a.10.b"));
    FieldRefSet fieldRefSet;
    {
        // Test resize.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        DeltaExecutor test(fromjson("{sf1: {a: true, l: 1}}"),
                           mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: [{a: {b: {c: 1}, c: 1}}]}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // When the index path is a prefix of a path in the diff and also involves numeric
        // components along the way. The numeric components should always be ignored.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {a: true, s0: {sa: {sb: {i: {d: 1} }}}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: [{a: {b: {c: 1, d: 1}, c: 1}}, 1]}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // When inserting a sub-object into array, and the sub-object diverges from the index path.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {a: true, u2: {b: 1}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: [{a: {b: {c: 1}, c: 1}}, 1, {b:1}]}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // When a common array path element is updated, but the paths diverge at the last element.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {a: true, s0: {sa: {d: {c: false} }}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: [{a: {b: {c: 1}}}, 1]}"));
        ASSERT_EQ(0, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
}

TEST(DeltaExecutorTest, ArraysAfterIndexPath) {
    BSONObj preImage(fromjson("{f1: {a: {b: [{c: 1}, 2]}}}"));
    UpdateIndexData indexData1, indexData2;
    constexpr bool mustCheckExistenceForInsertOperations = true;
    indexData1.addPath(FieldRef("p.a.b"));
    // 'UpdateIndexData' will canonicalize the path and remove all numeric components. So the '9'
    // and '10' components should not matter.
    indexData2.addPath(FieldRef("f1.9.a.10"));
    FieldRefSet fieldRefSet;
    {
        // Test resize.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        DeltaExecutor test(fromjson("{sf1: {sa: {sb: {a: true, l: 1}}}}"),
                           mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: [{c: 1}]}}}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // Updating a sub-array element.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {sb: {a: true, s0: {u: {c: 2}} }}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: [{c: 2}, 2]}}}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
    {
        // Updating an array element.
        auto doc = mutablebson::Document(preImage);
        UpdateExecutor::ApplyParams params(doc.root(), fieldRefSet);
        auto test = DeltaExecutor(fromjson("{sf1: {sa: {sb: {a: true, u0: 1 }}}}"),
                                  mustCheckExistenceForInsertOperations);
        auto result = test.applyUpdate(params);
        ASSERT_BSONOBJ_BINARY_EQ(params.element.getDocument().getObject(),
                                 fromjson("{f1: {a: {b: [1, 2]}}}"));
        ASSERT_EQ(2, getIndexAffectedFromLogEntry({&indexData1, &indexData2}, result.oplogEntry));
    }
}

}  // namespace
}  // namespace mongo
