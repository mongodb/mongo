/**
 * Killing a query during spilled group merging stops before the remaining partials are merged.
 *
 * @tags: [requires_persistence]
 */
import {withFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

describe("interrupt spilled group merging", function () {
    let conn;
    let db;
    const numDocs = 100;
    const pipeline = [{$group: {_id: "$k", c: {$sum: 1}}}];

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagSbeFull: true,
                featureFlagTimeSeriesInSbe: true,
                internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: 1,
                internalQuerySlotBasedExecutionHashAggMemoryCheckPerAdvanceAtMost: 1,
                internalQuerySlotBasedExecutionHashAggMemoryCheckPerAdvanceAtLeast: 1,
            },
        });
        assert.neq(conn, null);
        db = conn.getDB(jsTestName());
        assert.commandWorked(
            db.createCollection("block", {
                timeseries: {timeField: "t", metaField: "m"},
            }),
        );
        const docs = Array.from({length: numDocs}, (_, i) => ({
            _id: i,
            k: 0,
            m: i,
            t: new Date(0),
        }));
        assert.commandWorked(db.scalar.insertMany(docs));
        assert.commandWorked(db.block.insertMany(docs));
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
        }
    });

    for (const {name, collection, framework, stageName, failPointName} of [
        {
            name: "classic",
            collection: "scalar",
            framework: "forceClassicEngine",
            stageName: "$group",
            failPointName: "sleepInGroupSpillMergeLoop",
        },
        {
            name: "scalar SBE",
            collection: "scalar",
            framework: "trySbeEngine",
            stageName: "group",
            failPointName: "sleepInSbeGroupSpillMergeLoop",
        },
        {
            name: "block SBE",
            collection: "block",
            framework: "trySbeEngine",
            stageName: "block_group",
            failPointName: "sleepInSbeGroupSpillMergeLoop",
        },
    ]) {
        it(`kills ${name} during merging`, function () {
            runWithParamsAllNonConfigNodes(
                db,
                {
                    internalQueryFrameworkControl: framework,
                    internalQueryEnableAggressiveSpillsInGroup: true,
                },
                () => {
                    const explain = db[collection]
                        .explain("executionStats")
                        .aggregate(pipeline, {allowDiskUse: true});
                    const stage = getAggPlanStage(explain, stageName);
                    assert.neq(stage, null, explain);
                    assert(stage.usedDisk, explain);

                    const comment = `${jsTestName()}-${name}`;
                    let join;
                    try {
                        withFailPoint(
                            conn,
                            failPointName,
                            (fp) => {
                                join = startParallelShell(
                                    funWithArgs(
                                        function (dbName, collection, pipeline, comment) {
                                            assert.commandFailedWithCode(
                                                db.getSiblingDB(dbName).runCommand({
                                                    aggregate: collection,
                                                    pipeline,
                                                    allowDiskUse: true,
                                                    comment,
                                                    cursor: {},
                                                }),
                                                ErrorCodes.Interrupted,
                                            );
                                        },
                                        db.getName(),
                                        collection,
                                        pipeline,
                                        comment,
                                    ),
                                    conn.port,
                                );
                                // Each sleep enters shouldFail and execute; wait until the sleep begins.
                                fp.wait({timesEntered: 2});
                                const ops = db
                                    .getSiblingDB("admin")
                                    .aggregate([
                                        {$currentOp: {}},
                                        {$match: {"command.comment": comment}},
                                    ])
                                    .toArray();
                                assert.eq(ops.length, 1, {ops});
                                const parameter = `failpoint.${failPointName}`;
                                const before = assert.commandWorked(
                                    db.adminCommand({
                                        getParameter: 1,
                                        [parameter]: 1,
                                    }),
                                )[parameter].timesEntered;
                                assert.commandWorked(db.adminCommand({killOp: 1, op: ops[0].opid}));
                                const waitForQuery = join;
                                join = null;
                                waitForQuery();
                                const after = assert.commandWorked(
                                    db.adminCommand({
                                        getParameter: 1,
                                        [parameter]: 1,
                                    }),
                                )[parameter].timesEntered;
                                // Cancellation must be observed in the merge loop, not after draining it.
                                assert.lte((after - before) / 2, 1, {before, after, name});
                            },
                            {data: {millis: 100}},
                        );
                    } finally {
                        if (join) {
                            join();
                        }
                    }
                    assert.commandWorked(db.adminCommand({hello: 1}));
                },
            );
        });
    }
});
