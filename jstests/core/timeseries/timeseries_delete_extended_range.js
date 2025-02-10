/**
 * This test exercises deletes on time series collections with and without extended range
 * data, and verifies results and that predicates are generated correctly. Before 8.0, we don't
 * push down predicates on the time field at all, and also don't generate any predicates on the _id
 * field for buckets, even if there are predicates on the time field.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   # Arbitrary timeseries delete support is not present before 7.0
 *   requires_fcv_70
 * ]
 */

(function() {
load("jstests/libs/analyze_plan.js");

const timeFieldName = "time";
const metaFieldName = "tag";

let testDB = null;

function getTestDB() {
    if (!testDB) {
        testDB = db.getSiblingDB(jsTestName());
        assert.commandWorked(testDB.dropDatabase());
    }
    return testDB;
}

function prepareCollection({dbToUse, collName, initialDocList}) {
    if (!dbToUse) {
        dbToUse = getTestDB();
    }
    const coll = dbToUse.getCollection(collName);
    coll.drop();
    assert.commandWorked(dbToUse.createCollection(coll.getName(), {
        timeseries: {
            timeField: timeFieldName,
            metaField: metaFieldName,
        }
    }));
    assert.commandWorked(coll.insert(initialDocList));

    return coll;
}

function verifyResultDocs(coll, initialDocList, expectedResultDocs, nDeleted) {
    let resultDocs = coll.find().toArray();
    assert.eq(resultDocs.length, initialDocList.length - nDeleted, tojson(resultDocs));

    assert.eq(expectedResultDocs.length, resultDocs.length, tojson(resultDocs));
    assert.sameMembers(expectedResultDocs, resultDocs, tojson(resultDocs));
}

function verifyExplain({
    explain,
    residualFilter,
    nBucketsUnpacked,
}) {
    jsTestLog(`Explain: ${tojson(explain)}`);
    let foundStage = getPlanStage(explain.queryPlanner.winningPlan, "TS_MODIFY");
    assert.neq(
        null, foundStage, `The root TS_MODIFY stage not found in the plan: ${tojson(explain)}`);

    assert.eq("deleteMany", foundStage.opType, `TS_MODIFY opType is wrong: ${tojson(foundStage)}`);
    assert.eq({"control.closed": {$not: {$eq: true}}},
              foundStage.bucketFilter,
              `TS_MODIFY bucketFilter is wrong: ${tojson(foundStage)}`);
    assert.eq(residualFilter,
              foundStage.residualFilter,
              `TS_MODIFY residualFilter is wrong: ${tojson(foundStage)}`);

    const execStages = getExecutionStages(explain);
    assert.eq("TS_MODIFY", execStages[0].stage, `The root stage is wrong: ${tojson(execStages)}`);
    let tsModifyStage = execStages[0];
    assert.eq(
        "TS_MODIFY", tsModifyStage.stage, `Can't find TS_MODIFY stage: ${tojson(execStages)}`);

    assert.eq(nBucketsUnpacked,
              tsModifyStage.nBucketsUnpacked,
              `Got wrong nBucketsUnpacked ${tojson(tsModifyStage)}`);
}

function getCallerName() {
    return `${new Error().stack.split('\n')[2].split('@')[0]}`;
}

function makeDeleteCommand(coll, filter) {
    return {delete: coll.getName(), deletes: [{q: filter, limit: 0}]};
}

/**
 * Verifies that a delete returns the expected result(s) 'res'.
 *
 * - initialDocList: The initial documents in the collection.
 * - cmd.filter: The filter for the findAndModify command.
 * - res.expectedResultDocs: The expected documents in the collection after the delete.
 * - res.nDeleted: The expected number of documents deleted.
 * - res.residualFilter: The expected residual filter of the TS_MODIFY stage.
 * - res.nBucketsUnpacked: The expected number of buckets unpacked by the TS_MODIFY stage.
 */
function testDelete({
    initialDocList,
    cmd: {filter},
    res: {
        expectedResultDocs,
        nDeleted,
        residualFilter,
        nBucketsUnpacked,
    }
}) {
    const callerName = getCallerName();
    jsTestLog(`Running ${callerName}(${tojson(arguments[0])})`);

    const coll = prepareCollection({
        collName: callerName,
        initialDocList: initialDocList,
    });

    const deleteCmd = makeDeleteCommand(coll, filter);
    jsTestLog(`Running delete: ${tojson(deleteCmd)}`);

    const explainRes =
        assert.commandWorked(coll.runCommand({explain: deleteCmd, verbosity: "executionStats"}));
    verifyExplain({
        explain: explainRes,
        residualFilter: residualFilter,
        nBucketsUnpacked: nBucketsUnpacked,
    });

    const res = assert.commandWorked(testDB.runCommand(deleteCmd));
    jsTestLog(`delete result: ${tojson(res)}`);
    assert.eq(nDeleted, res.n, tojson(res));
    verifyResultDocs(coll, initialDocList, expectedResultDocs, nDeleted);
}

// A set of documents that contain extended range data before and after the epoch.
const extendedRangeDocs = [
    {
        [timeFieldName]: ISODate("1968-01-01T00:00:00Z"),
        [metaFieldName]: 1,
        _id: 0,
        a: 10,
    },
    {
        [timeFieldName]: ISODate("1971-01-01T00:00:00Z"),
        [metaFieldName]: 1,
        _id: 1,
        a: 10,
    },
    {
        [timeFieldName]: ISODate("2035-01-01T00:00:00Z"),
        [metaFieldName]: 1,
        _id: 2,
        a: 10,
    },
    {
        [timeFieldName]: ISODate("2040-01-01T00:00:00Z"),
        [metaFieldName]: 1,
        _id: 3,
        a: 10,
    },
];

// A set of documents that does not contain extended range data.
const normalDocs = [
    {
        [timeFieldName]: ISODate("1972-01-01T00:00:00Z"),
        [metaFieldName]: 1,
        _id: 0,
        a: 10,
    },
    {
        [timeFieldName]: ISODate("1975-01-01T00:00:00Z"),
        [metaFieldName]: 1,
        _id: 1,
        a: 10,
    },
    {
        [timeFieldName]: ISODate("2035-01-01T00:00:00Z"),
        [metaFieldName]: 1,
        _id: 2,
        a: 10,
    },
    {
        [timeFieldName]: ISODate("2038-01-01T00:00:00Z"),
        [metaFieldName]: 1,
        _id: 3,
        a: 10,
    },
];

(function testDelete_extendedRange() {
    testDelete({
        initialDocList: extendedRangeDocs,
        cmd: {
            filter: {[timeFieldName]: {$gt: ISODate("1972-01-01T00:00:00Z")}},
        },
        res: {
            expectedResultDocs: [
                {
                    [timeFieldName]: ISODate("1968-01-01T00:00:00Z"),
                    [metaFieldName]: 1,
                    _id: 0,
                    a: 10,
                },
                {
                    [timeFieldName]: ISODate("1971-01-01T00:00:00Z"),
                    [metaFieldName]: 1,
                    _id: 1,
                    a: 10,
                },
            ],
            residualFilter: {"time": {"$gt": ISODate("1972-01-01T00:00:00Z")}},
            nBucketsUnpacked: 4,
            nDeleted: 2
        },
    });
})();

(function testDelete_normal() {
    testDelete({
        initialDocList: normalDocs,
        cmd: {
            filter: {[timeFieldName]: {$gt: ISODate("1972-01-01T00:00:00Z")}},
        },
        res: {
            expectedResultDocs: [
                {
                    [timeFieldName]: ISODate("1972-01-01T00:00:00Z"),
                    [metaFieldName]: 1,
                    _id: 0,
                    a: 10,
                },
            ],
            residualFilter: {"time": {"$gt": ISODate("1972-01-01T00:00:00Z")}},
            nBucketsUnpacked: 4,
            nDeleted: 3
        },
    });
})();

(function testDelete_normalExtendedPredicateAll() {
    testDelete({
        initialDocList: normalDocs,
        cmd: {
            filter: {[timeFieldName]: {$gt: ISODate("1950-01-01T00:00:00Z")}},
        },
        res: {
            expectedResultDocs: [],
            // The bucket filter is trivially true (empty predicate).
            // All the documents in the collection are after 1950.
            residualFilter: {"time": {"$gt": ISODate("1950-01-01T00:00:00Z")}},
            nBucketsUnpacked: 4,
            nDeleted: 4
        },
    });
})();

(function testDelete_normalExtendedPredicateNone() {
    testDelete({
        initialDocList: normalDocs,
        cmd: {
            filter: {[timeFieldName]: {$lt: ISODate("1950-01-01T00:00:00Z")}},
        },
        res: {
            expectedResultDocs: normalDocs,
            // None of the documents in the collection can be before 1950.
            residualFilter: {"time": {"$lt": ISODate("1950-01-01T00:00:00Z")}},
            nBucketsUnpacked: 4,
            nDeleted: 0
        },
    });
})();
})();
