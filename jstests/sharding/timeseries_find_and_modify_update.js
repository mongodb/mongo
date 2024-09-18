/**
 * Tests findAndModify remove on a sharded timeseries collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # To avoid burn-in tests in in-memory build variants
 *   requires_persistence,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # TODO SERVER-76583: Remove following two tags.
 *   does_not_support_retryable_writes,
 *   requires_non_retryable_writes,
 * ]
 */

import {
    doc1_a_nofields,
    doc2_a_f101,
    doc3_a_f102,
    doc4_b_f103,
    doc5_b_f104,
    doc6_c_f105,
    doc7_c_f106,
    generateTimeValue,
    makeBucketFilter,
    metaFieldName,
    setUpShardedCluster,
    tearDownShardedCluster,
    testFindOneAndUpdateOnShardedCollection,
    timeFieldName
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";

const docs = [
    doc1_a_nofields,
    doc2_a_f101,
    doc3_a_f102,
    doc4_b_f103,
    doc5_b_f104,
    doc6_c_f105,
    doc7_c_f106,
];

setUpShardedCluster();

(function testSortOptionFailsOnShardedCollection() {
    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {f: {$gt: 100}}, update: {f: 200}, sort: {f: 1}},
        res: {errorCode: ErrorCodes.InvalidOptions},
    });
})();

(function testProjectOptionHonoredOnShardedCollection() {
    const returnDoc =
        {_id: doc7_c_f106._id, [timeFieldName]: doc7_c_f106[timeFieldName], f: doc7_c_f106.f};
    const copyDocs = docs.map(doc => Object.assign({}, doc));
    const resultDocList = copyDocs.filter(doc => doc._id !== 7);
    resultDocList.push(Object.assign({}, doc7_c_f106, {f: 300}));

    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        cmd: {
            filter: {f: 106},
            update: {$set: {f: 300}},
            fields: {_id: 1, [timeFieldName]: 1, f: 1}
        },
        res: {
            resultDocList: resultDocList,
            returnDoc: returnDoc,
            writeType: "twoPhaseProtocol",
            dataBearingShard: "other",
        },
    });
})();

// Verifies that the collation is properly propagated to the bucket-level filter when the
// query-level collation overrides the collection default collation. This is a two phase update due
// to the user-specified collation.
(function testTwoPhaseUpdateCanHonorCollationOnShardedCollection() {
    const returnDoc = Object.assign({}, doc3_a_f102, {[metaFieldName]: "C"});
    const copyDocs = docs.map(doc => Object.assign({}, doc));
    const resultDocList = copyDocs.filter(doc => doc._id !== 3);
    resultDocList.push(returnDoc);

    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        cmd: {
            filter: {[metaFieldName]: "a", f: 102},
            // This also excercises the shard key update in the two phase update. The two phase
            // update will run inside an internal transaction. So we don't need to run this update
            // in a transaction.
            update: {$set: {[metaFieldName]: "C"}},
            returnNew: true,
            // caseInsensitive collation
            collation: {locale: "en", strength: 2}
        },
        res: {
            resultDocList: resultDocList,
            returnDoc: returnDoc,
            writeType: "twoPhaseProtocol",
            dataBearingShard: "primary",
        },
    });
})();

// Query on the meta field and 'f' field leads to a targeted update but no measurement is updated.
(function testTargetedUpdateByNonMatchingFilter() {
    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {[metaFieldName]: "C", f: 17}, update: {$set: {_id: 1000}}},
        res: {
            resultDocList: docs,
            writeType: "targeted",
            dataBearingShard: "other",
            rootStage: "TS_MODIFY",
            bucketFilter: makeBucketFilter({"meta": {$eq: "C"}}, {
                $and: [
                    {"control.min.f": {$_internalExprLte: 17}},
                    {"control.max.f": {$_internalExprGte: 17}},
                ]
            }),
            residualFilter: {f: {$eq: 17}},
            nBucketsUnpacked: 0,
            nMatched: 0,
            nModified: 0,
        },
    });
})();

// Query on the 'f' field leads to zero measurement update.
(function testTwoPhaseUpdateByNonMatchingFilter() {
    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {f: 17}, update: {$set: {_id: 1000}}},
        res: {
            resultDocList: docs,
            writeType: "twoPhaseProtocol",
            dataBearingShard: "none",
            rootStage: "TS_MODIFY",
            bucketFilter: makeBucketFilter({
                $and: [
                    {"control.min.f": {$_internalExprLte: 17}},
                    {"control.max.f": {$_internalExprGte: 17}},
                ]
            }),
            residualFilter: {f: {$eq: 17}},
        },
    });
})();

// Query on the meta field and 'f' field leads to a targeted update when the meta field is included
// in the shard key. Pipeline-style update.
(function testTargetedUpdateByShardKeyAndFieldFilter() {
    const modifiedDoc = {_id: 1000, [metaFieldName]: "B", [timeFieldName]: generateTimeValue(4)};
    const copyDocs = docs.map(doc => Object.assign({}, doc));
    const resultDocList = copyDocs.filter(doc => doc._id !== 4);
    resultDocList.push(modifiedDoc);

    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        cmd: {
            filter: {[metaFieldName]: "B", f: 103},
            update: [{$set: {_id: 1000}}, {$unset: "f"}],
        },
        res: {
            resultDocList: resultDocList,
            returnDoc: doc4_b_f103,
            writeType: "targeted",
            dataBearingShard: "other",
            rootStage: "TS_MODIFY",
            bucketFilter: makeBucketFilter({"meta": {$eq: "B"}}, {
                $and: [
                    {"control.min.f": {$_internalExprLte: 103}},
                    {"control.max.f": {$_internalExprGte: 103}},
                ]
            }),
            residualFilter: {f: {$eq: 103}},
            nBucketsUnpacked: 1,
            nReturned: 1,
        },
    });
})();

// Query on the meta field and 'f' field leads to a targeted update but fails because of unset of
// the time field.
(function testTargetedUpdateByShardKeyAndFieldFilter() {
    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        cmd: {
            filter: {[metaFieldName]: "B", f: 103},
            update: [{$set: {_id: 1000}}, {$unset: timeFieldName}],
        },
        res: {errorCode: ErrorCodes.BadValue},
    });
})();

const replacementDoc = {
    _id: 1000,
    [metaFieldName]: "A",
    [timeFieldName]: generateTimeValue(0),
    f: 2000
};

// Query on the meta field and 'f' field leads to a two phase update when the meta field is not
// included in the shard key. Replacement-style update. The new time value makes the measurement
// belong to a different shard but the time field is not specified in the query and so, this update
// should fail.
(function testTwoPhaseShardKeyUpdateByMetaAndFieldFilterButNoShardKey() {
    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        includeMeta: false,
        cmd: {filter: {[metaFieldName]: "B", f: 103}, update: replacementDoc},
        res: {errorCode: 7717803},
    });
})();

// Query on the 'f' field leads to a two phase update. Replacement-style update. The meta value
// makes the measurement belong to a different shard and the request runs in a transaction. This
// should succeed.
(function testTwoPhaseShardKeyUpdateByFieldFilter() {
    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        startTxn: true,
        cmd: {filter: {f: 106}, update: replacementDoc, returnNew: true},
        // Don't validate the resultDocList because we don't know which doc will be replaced.
        res: {
            returnDoc: replacementDoc,
            writeType: "twoPhaseProtocol",
            dataBearingShard: "other",
        },
    });
})();

// Query on the meta field and 'f' field leads to a targeted update when the meta field is not
// included in the shard key. Replacement-style update. The new meta value makes the measurement
// belong to a different shard but it does not run in a transaction and it should fail.
(function testTargetedShardKeyUpdateByMetaAndFieldFilterButNotInTxn() {
    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {[metaFieldName]: "B", f: 103}, update: replacementDoc, returnNew: true},
        res: {errorCode: ErrorCodes.IllegalOperation},
    });
})();

// Query on the meta field and 'f' field leads to a targeted update when the meta field is included
// in the shard key. Replacement-style update. The new meta value makes the measurement belong to a
// different shard. This should run in a transaction.
(function testTargetedShardKeyUpdateByMetaAndFieldFilter() {
    const copyDocs = docs.map(doc => Object.assign({}, doc));
    const resultDocList = copyDocs.filter(doc => doc._id !== 4);
    resultDocList.push(replacementDoc);

    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        startTxn: true,
        cmd: {filter: {[metaFieldName]: "B", f: 103}, update: replacementDoc, returnNew: true},
        res: {
            resultDocList: resultDocList,
            returnDoc: replacementDoc,
            writeType: "targeted",
            dataBearingShard: "other",
            // We can't verify explain output because explain can't run in a transaction.
        },
    });
})();

// Meta filter matches all docs with tag: "B" but only update one. The replacement doc has tag: "A"
// and so, the measurement will be moved to a different shard. This should run in a transaction and
// succeed.
(function testTargetedShardKeyUpdateByMetaFilter() {
    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        startTxn: true,
        cmd: {filter: {[metaFieldName]: "B"}, update: replacementDoc, returnNew: true},
        // Don't validate the resultDocList because we don't know which doc will be replaced.
        res: {
            returnDoc: replacementDoc,
            writeType: "targeted",
            dataBearingShard: "other",
        },
    });
})();

// The update is targeted but there's actually no match. So, the update becomes an upsert.
(function testTargetedPipelineUpsertByMetaAndFieldFilter() {
    const returnDoc = Object.assign(
        {}, {_id: -100, [metaFieldName]: "B", [timeFieldName]: generateTimeValue(10), f: 2345});
    const resultDocList = docs.map(doc => Object.assign({}, doc));
    resultDocList.push(returnDoc);

    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        cmd: {
            filter: {[metaFieldName]: "B", f: 2345},
            update: [{$set: {_id: -100}}, {$set: {[timeFieldName]: generateTimeValue(10)}}],
            upsert: true,
            returnNew: true,
        },
        res: {
            resultDocList: resultDocList,
            returnDoc: returnDoc,
            writeType: "targeted",
            dataBearingShard: "other",
            bucketFilter: makeBucketFilter({"meta": {$eq: "B"}}, {
                $and: [
                    {"control.min.f": {$_internalExprLte: 2345}},
                    {"control.max.f": {$_internalExprGte: 2345}},
                ]
            }),
            residualFilter: {f: {$eq: 2345}},
            nBucketsUnpacked: 0,
            nMatched: 0,
            nModified: 0,
            nUpserted: 1,
        },
    });
})();

// The update is targeted but there's actually no match. The update becomes an upsert but the
// replacement document has a different shard key value.
(function testTargetedReplacementUpsertByMetaAndFieldFilter() {
    const replacementDoc = Object.assign(
        {}, {_id: -100, [metaFieldName]: "A", [timeFieldName]: generateTimeValue(10), f: 2345});
    const resultDocList = docs.map(doc => Object.assign({}, doc));
    resultDocList.push(replacementDoc);

    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        startTxn: true,
        cmd: {
            filter: {[metaFieldName]: "B", f: 2345},
            update: replacementDoc,
            upsert: true,
            returnNew: true,
        },
        res: {
            resultDocList: resultDocList,
            returnDoc: replacementDoc,
            writeType: "targeted",
            dataBearingShard: "other",
            nUpserted: 1,
        },
    });
})();

(function testTwoPhaseReplacementUpsertByFieldFilter() {
    const replacementDoc = Object.assign(
        {}, {_id: -100, [metaFieldName]: "A", [timeFieldName]: generateTimeValue(10), f: 2345});
    const resultDocList = docs.map(doc => Object.assign({}, doc));
    resultDocList.push(replacementDoc);

    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        cmd: {
            filter: {f: {$gt: 500}},
            update: replacementDoc,
            upsert: true,
            returnNew: true,
        },
        res: {
            resultDocList: resultDocList,
            returnDoc: replacementDoc,
            writeType: "twoPhaseProtocol",
            // For a two-phase upsert, no shard will get the targeted findAndModify update command.
            // Instead, one of them will get an insert command.
            dataBearingShard: "none",
            nUpserted: 1,
        },
    });
})();

// Query on the time field leads to a targeted update when the time field is included in the shard
// key.
(function testTargetedUpdateByTimeShardKeyFilter() {
    const modifiedDoc = Object.assign({}, doc6_c_f105, {f: 1234});
    const copyDocs = docs.map(doc => Object.assign({}, doc));
    const resultDocList = copyDocs.map(doc => doc._id !== doc6_c_f105._id ? doc : modifiedDoc);

    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        includeMeta: false,
        cmd: {filter: {[timeFieldName]: doc6_c_f105[timeFieldName]}, update: {$set: {f: 1234}}},
        res: {
            resultDocList: resultDocList,
            returnDoc: doc6_c_f105,
            writeType: "targeted",
            dataBearingShard: "other",
            rootStage: "TS_MODIFY",
            bucketFilter: makeBucketFilter({
                $and: [
                    // The bucket's _id encodes the time info and so the bucket filter will include
                    // the _id range filter.
                    {"_id": {"$lte": ObjectId("43b71b80ffffffffffffffff")}},
                    {"_id": {"$gte": ObjectId("43b70d700000000000000000")}},
                    {
                        [`control.max.${timeFieldName}`]:
                            {$_internalExprGte: doc6_c_f105[timeFieldName]}
                    },
                    // +1 hour
                    {
                        [`control.min.${timeFieldName}`]:
                            {$_internalExprGte: ISODate("2005-12-31T23:00:00Z")}
                    },
                    // -1 hour
                    {
                        [`control.max.${timeFieldName}`]:
                            {$_internalExprLte: ISODate("2006-01-01T01:00:00Z")}
                    },
                    {
                        [`control.min.${timeFieldName}`]:
                            {$_internalExprLte: doc6_c_f105[timeFieldName]}
                    },
                ]
            }),
            residualFilter: {[timeFieldName]: {$eq: doc6_c_f105[timeFieldName]}},
            nBucketsUnpacked: 1,
            nMatched: 1,
            nModified: 1,
        },
    });
})();

// Query on the time field leads to a two phase update when the time field is not included in the
// shard key.
(function testTwoPhaseUpdateByTimeFieldFilter() {
    const modifiedDoc = Object.assign({}, doc7_c_f106, {f: 107});
    const copyDocs = docs.map(doc => Object.assign({}, doc));
    const resultDocList = copyDocs.map(doc => doc._id !== doc7_c_f106._id ? doc : modifiedDoc);

    testFindOneAndUpdateOnShardedCollection({
        initialDocList: docs,
        cmd: {filter: {[timeFieldName]: doc7_c_f106[timeFieldName]}, update: {$inc: {f: 1}}},
        res: {
            resultDocList: resultDocList,
            returnDoc: doc7_c_f106,
            writeType: "twoPhaseProtocol",
            dataBearingShard: "other",
        },
    });
})();

tearDownShardedCluster();
