/**
 * Tests classic engine kills queries when path arrayness assumptions are invalidated.
 * Covers all subclasses of RequiresCollectionStage, besides RecordStoreFastCountStage.
 * Uses a failpoint to trigger invalidation unconditionally.
 * RecordStoreFastCountStage returns EOF immediately.
 *
 * Also verifies a more realistic case where a match can be pushed before a project.
 * In that case, the path arrayness's list of non-array paths is used to invalidate.
 *
 * @tags: [requires_fcv_90]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const NUM_DOCS = 10;

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPathArrayness: true,
        internalEnablePathArrayness: true,
        internalQueryExecYieldIterations: 1,
        internalQueryFrameworkControl: "forceClassicEngine",
        featureFlagTimeseriesUpdatesSupport: true,
        featureFlagImprovedDepsAnalysis: true,
    },
});
assert.neq(null, conn, "mongod failed to start");

const db = conn.getDB("test");

function setupColl(name) {
    const coll = db[name];
    coll.drop();
    for (let i = 0; i < NUM_DOCS; i++) {
        assert.commandWorked(coll.insert({_id: i, a: i, b: i, val: "hello world"}));
    }
    return coll;
}

const kKilledMsg = "non-array path became multikey during yield";

function runPhases({runQuery, isWriteCmd}) {
    const before = db.adminCommand({serverStatus: 1}).metrics.query.pathArrayness.queriesFailedDueToInvalidation;
    const fp = configureFailPoint(db, "pathArraynessYieldInvalidation", {}, {times: 1});
    try {
        // Verify query is killed with the path-arrayness invalidation message.
        if (isWriteCmd) {
            const res = runQuery();
            assert.commandFailedWithCode(res, ErrorCodes.QueryPlanKilled);
            assert(res.writeErrors[0].errmsg.includes(kKilledMsg), res.writeErrors[0].errmsg);
        } else {
            const err = assert.throws(runQuery);
            assert.eq(err.code, ErrorCodes.QueryPlanKilled, err.message);
            assert(err.message.includes(kKilledMsg), err.message);
        }
    } finally {
        fp.off();
    }
    const after = db.adminCommand({serverStatus: 1}).metrics.query.pathArrayness.queriesFailedDueToInvalidation;
    assert.eq(after, before + 1, "expected invalidation counter to be incremented", {before, after});
}

{
    jsTest.log("Testing CollectionScan");
    const coll = setupColl("collscan");
    runPhases({
        runQuery: () => coll.find({}).hint({$natural: 1}).toArray(),
    });
}

{
    jsTest.log("Testing FetchStage");
    const coll = setupColl("fetch");
    assert.commandWorked(coll.createIndex({a: 1}));
    runPhases({
        // Non-covered query: IXSCAN -> FETCH
        runQuery: () =>
            coll
                .find({a: {$gte: 0}})
                .hint({a: 1})
                .toArray(),
    });
}

{
    jsTest.log("Testing MultiIteratorStage");
    const coll = db["multi_iterator"];
    coll.drop();
    // Need more docs for MultiIteratorStage.
    for (let i = 0; i < 200; i++) {
        assert.commandWorked(coll.insert({_id: i, a: i, b: i, val: "hello world"}));
    }
    runPhases({
        runQuery: () => coll.aggregate([{$sample: {size: 3}}]).toArray(),
    });
}

{
    jsTest.log("Testing RequiresIndexStage");
    const coll = setupColl("index_scan");
    assert.commandWorked(coll.createIndex({a: 1}));
    runPhases({
        runQuery: () =>
            coll
                .find({a: {$gte: 0}}, {a: 1, _id: 0})
                .hint({a: 1})
                .toArray(),
    });
}

{
    jsTest.log("Testing TextOrStage");
    const coll = setupColl("text_or");
    assert.commandWorked(coll.createIndex({val: "text"}));
    runPhases({
        runQuery: () => coll.find({$text: {$search: "hello"}}, {score: {$meta: "textScore"}}).toArray(),
    });
}

{
    jsTest.log("Testing DeleteStage");
    const coll = setupColl("delete_stage");
    runPhases({
        isWriteCmd: true,
        runQuery: () =>
            db.runCommand({
                delete: coll.getName(),
                deletes: [{q: {a: {$gte: NUM_DOCS + 1}}, limit: 0}],
            }),
    });
}

{
    jsTest.log("Testing UpdateStage");
    const coll = setupColl("update_stage");
    runPhases({
        isWriteCmd: true,
        runQuery: () =>
            db.runCommand({
                update: coll.getName(),
                updates: [{q: {}, u: {$set: {x: 1}}, multi: true}],
            }),
    });
}

{
    jsTest.log("Testing TimeseriesModifyStage");
    const tsCollName = "ts_modify";
    db[tsCollName].drop();
    assert.commandWorked(
        db.createCollection(tsCollName, {
            timeseries: {timeField: "t", metaField: "meta"},
        }),
    );
    const tsColl = db[tsCollName];
    const baseTime = new Date();
    for (let i = 0; i < NUM_DOCS; i++) {
        assert.commandWorked(tsColl.insert({t: new Date(baseTime.getTime() + i * 1000), meta: i, v: i}));
    }
    runPhases({
        isWriteCmd: true,
        runQuery: () =>
            db.runCommand({
                update: tsCollName,
                updates: [{q: {}, u: {$set: {meta: -1}}, multi: true}],
            }),
    });
}

{
    jsTest.log("Testing MultiPlanStage");
    const coll = setupColl("multi_plan");
    // Two indexes.
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    runPhases({
        runQuery: () => coll.find({a: {$gte: 0}, b: {$gte: 0}}).toArray(),
    });
}

{
    jsTest.log("Testing RequiresAllIndicesStage (SubplanStage)");
    const coll = setupColl("requires_all_indices");
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    runPhases({
        runQuery: () => coll.find({$or: [{a: 1}, {b: 2}]}).toArray(),
    });
}

{
    jsTest.log("Testing aggregate $match pushdown yield invalidation (real multikey insert)");
    const collName = "agg_match_pushdown";
    db[collName].drop();

    const docs = [];
    for (let i = 0; i < 100; i++) {
        docs.push({_id: i, b: {c: 42}});
    }
    assert.commandWorked(db[collName].insertMany(docs));
    // Non-multikey index.
    assert.commandWorked(db[collName].createIndex({"b.c": 1}));

    const pipeline = [{$addFields: {a: "$b.c"}}, {$match: {a: 42}}];

    const pauseFp = configureFailPoint(db, "setYieldAllLocksHang");
    const awaitShell = startParallelShell(
        funWithArgs(
            function (dbName, collName, pipeline, kKilledMsg) {
                const coll = db.getSiblingDB(dbName)[collName];
                const err = assert.throws(() => coll.aggregate(pipeline).toArray());
                assert.eq(err.code, ErrorCodes.QueryPlanKilled, err.message);
                assert(err.message.includes(kKilledMsg), err.message);
            },
            db.getName(),
            collName,
            pipeline,
            kKilledMsg,
        ),
        conn.port,
    );
    // Hang in yield.
    pauseFp.wait();
    // Break arrayness assumption.
    assert.commandWorked(db[collName].insert({_id: 200, b: [{c: 42}]}));
    // Continue in yield.
    pauseFp.off();
    awaitShell();
}

MongoRunner.stopMongod(conn);
