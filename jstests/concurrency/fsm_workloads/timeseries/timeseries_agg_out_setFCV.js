/**
 * Concurrency FSM workload that runs $out from normal and timeseries source collections to normal
 * and timeseries output collections while concurrently toggling FCV between lastLTS and latest.
 * The output collection is randomly dropped to exercise both the "create new" and "replace
 * existing" code paths. On success, verifies data correctness, secondary index preservation,
 * and output collection type.
 *
 * Designed to exercise timeseries viewless upgrade/downgrade interacting with $out.
 * TODO(SERVER-114573): Consider removing this test once 9.0 becomes lastLTS.
 *
 * @tags: [
 *   requires_timeseries,
 *   # FCV toggling requires all nodes on the latest binary.
 *   multiversion_incompatible,
 *   # TODO (SERVER-104171) Remove the 'assumes_balancer_off' tag
 *   assumes_balancer_off,
 *   # Runs setFCV, which can interfere with other tests.
 *   incompatible_with_concurrency_simultaneous,
 *   runs_set_fcv,
 *   # $out requires non-retryable writes.
 *   requires_non_retryable_writes,
 *   # $out supports sharded source collections but not sharded output collections.
 *   assumes_unsharded_collection,
 * ]
 */
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";

export const $config = (function () {
    const prefix = jsTestName();
    const timeFieldName = "time";
    const metaFieldName = "tag";
    const numDocs = 50;

    // Collection names.
    const normalSourceName = prefix + "_normal_src";
    const tsSourceName = prefix + "_ts_src";

    // Secondary index added to the output collection before $out to verify index cloning.
    const secondaryIndexSpec = {value: 1};
    const secondaryIndexName = "value_1";

    // Each thread gets its own output collection names to avoid conflicts between threads.
    function outCollName(tid, suffix) {
        return prefix + "_out_" + tid + "_" + suffix;
    }

    // Build the test documents that will be inserted into both source collections.
    function generateDocs() {
        const docs = [];
        for (let i = 0; i < numDocs; i++) {
            docs.push({
                [timeFieldName]: new Date(ISODate("2024-01-01T00:00:00.000Z").getTime() + i * 1000),
                [metaFieldName]: i,
                value: i,
            });
        }
        return docs;
    }

    // Verify the output collection has the correct data, indexes, and type after a successful $out.
    function verifyOutput(db, outName, hasSecondaryIndex, timeseriesOut) {
        // Verify data correctness: the value and tag fields should be preserved by $out
        // (only the time field is modified by the $set stage in the pipeline).
        assert.soon(() => {
            try {
                const docs = db[outName]
                    .find({}, {value: 1, [metaFieldName]: 1, _id: 0})
                    .sort({value: 1})
                    .toArray();
                assert.eq(docs.length, numDocs, `Expected ${numDocs} documents in ${outName}, got ${docs.length}`);
                for (let i = 0; i < numDocs; i++) {
                    assert.eq(docs[i].value, i, `Unexpected value at index ${i} in ${outName}`);
                    assert.eq(docs[i][metaFieldName], i, `Unexpected tag at index ${i} in ${outName}`);
                }
                return true;
            } catch (e) {
                jsTestLog(`Data verification retry on ${outName}: ${e}`);
                return false;
            }
        }, `Data verification failed on ${outName}`);

        // Verify that $out preserved the secondary index from the original output collection.
        if (hasSecondaryIndex) {
            assert.soon(() => {
                try {
                    const indexes = db[outName].getIndexes();
                    const found = indexes.some((idx) => idx.name === secondaryIndexName);
                    assert(
                        found,
                        `Secondary index '${secondaryIndexName}' not preserved by $out on ` +
                            `${outName}. Indexes: ${tojson(indexes)}`,
                    );
                    return true;
                } catch (e) {
                    jsTestLog(`Index verification retry on ${outName}: ${e}`);
                    return false;
                }
            }, `Index verification failed on ${outName}`);
        }

        // Verify collection type and timeseries options. FCV transitions only toggle between
        // viewful and viewless representations — they never change the fundamental collection
        // type, so these checks are stable across concurrent FCV changes.
        assert.soon(() => {
            try {
                const infos = db.getCollectionInfos({name: outName});
                assert.eq(infos.length, 1, `Expected 1 collection info for ${outName}, got ${tojson(infos)}`);
                const info = infos[0];
                if (timeseriesOut) {
                    assert.eq(
                        info.type,
                        "timeseries",
                        `Expected timeseries collection type for ${outName}, got ${tojson(info)}`,
                    );
                    assert.eq(
                        info.options.timeseries.timeField,
                        timeFieldName,
                        `Unexpected timeField on ${outName}: ${tojson(info.options.timeseries)}`,
                    );
                    assert.eq(
                        info.options.timeseries.metaField,
                        metaFieldName,
                        `Unexpected metaField on ${outName}: ${tojson(info.options.timeseries)}`,
                    );
                } else {
                    assert.neq(
                        info.type,
                        "timeseries",
                        `Expected non-timeseries collection for ${outName}, got ${tojson(info)}`,
                    );
                }
                return true;
            } catch (e) {
                jsTestLog(`Collection type verification retry on ${outName}: ${e}`);
                return false;
            }
        }, `Collection type verification failed on ${outName}`);
    }

    // Run $out, tolerate expected concurrent errors, and verify the output on success.
    function runOutAndVerify(db, sourceColl, outName, timeseriesOut) {
        // Randomly drop to test the "doesn't exist" case. Otherwise, if the output collection
        // already exists, add a secondary index to verify that $out clones it.
        let hasSecondaryIndex = false;
        if (Random.rand() < 0.3) {
            db[outName].drop();
        } else if (db.getCollectionNames().includes(outName)) {
            assert.soon(() => {
                const res = db[outName].createIndex(secondaryIndexSpec);
                return res.ok;
            }, `Failed to create secondary index on ${outName}`);
            hasSecondaryIndex = true;
        }

        let pipeline = [{$set: {[timeFieldName]: new Date()}}];
        if (timeseriesOut) {
            pipeline.push({
                $out: {
                    db: db.getName(),
                    coll: outName,
                    timeseries: {timeField: timeFieldName, metaField: metaFieldName},
                },
            });
        } else {
            pipeline.push({$out: outName});
        }

        const allowedErrors = [
            // Source, tmp, or output collection is timeseries and concurrently
            // upgraded/downgraded during $out execution.
            ErrorCodes.InterruptedDueToTimeseriesUpgradeDowngrade,
        ];

        const res = db.runCommand({
            aggregate: sourceColl,
            pipeline: pipeline,
            cursor: {},
        });

        if (res.ok) {
            verifyOutput(db, outName, hasSecondaryIndex, timeseriesOut);
        } else {
            // TODO(SERVER-123600): Remove the recordIdsReplicated workaround.
            if (
                (res.code === ErrorCodes.CommandNotSupported || res.code === ErrorCodes.CommandFailed) &&
                (res.errmsg || "").includes("recordIdsReplicated")
            ) {
                jsTestLog(`Ignoring expected $out error during FCV transition: ${tojson(res)}`);
                return;
            }

            assert.commandFailedWithCode(res, allowedErrors);
        }
    }

    const states = {
        init: function (db, collName) {},

        setFCV: function (db, collName) {
            const fcvValues = [lastLTSFCV, latestFCV];
            const targetFCV = fcvValues[Random.randInt(2)];
            jsTestLog("Executing FCV state, setting to:" + targetFCV);
            try {
                assert.commandWorked(
                    db.adminCommand({
                        setFeatureCompatibilityVersion: targetFCV,
                        confirm: true,
                    }),
                );
            } catch (e) {
                if (handleRandomSetFCVErrors(e, targetFCV)) return;
                throw e;
            }
            jsTestLog("setFCV state finished");
        },

        // $out from normal collection to normal collection.
        outNormalToNormal: function (db, collName) {
            const outName = outCollName(this.tid, "n2n");
            jsTestLog(`Running $out: [normal -> normal] ${normalSourceName} -> ${outName}`);
            runOutAndVerify(db, normalSourceName, outName, /*timeseriesOut=*/ false);
        },

        // $out from normal collection to timeseries collection.
        outNormalToTimeseries: function (db, collName) {
            const outName = outCollName(this.tid, "n2ts");
            jsTestLog(`Running $out: [normal -> timeseries] ${normalSourceName} -> ${outName}`);
            runOutAndVerify(db, normalSourceName, outName, /*timeseriesOut=*/ true);
        },

        // $out from timeseries collection to normal collection.
        outTimeseriesToNormal: function (db, collName) {
            const outName = outCollName(this.tid, "ts2n");
            jsTestLog(`Running $out: [timeseries -> normal] ${tsSourceName} -> ${outName}`);
            runOutAndVerify(db, tsSourceName, outName, /*timeseriesOut=*/ false);
        },

        // $out from timeseries collection to timeseries collection.
        outTimeseriesToTimeseries: function (db, collName) {
            const outName = outCollName(this.tid, "ts2ts");
            jsTestLog(`Running $out: [timeseries -> timeseries] ${tsSourceName} -> ${outName}`);
            runOutAndVerify(db, tsSourceName, outName, /*timeseriesOut=*/ true);
        },
    };

    const setup = function (db, collName, cluster) {
        const docs = generateDocs();

        // Create the normal source collection and insert data.
        assert.commandWorked(db[normalSourceName].insertMany(docs));

        // Create the timeseries source collection and insert data.
        assert.commandWorked(
            db.createCollection(tsSourceName, {
                timeseries: {timeField: timeFieldName, metaField: metaFieldName},
            }),
        );
        assert.commandWorked(db[tsSourceName].insertMany(docs));
    };

    const teardown = function (db, collName, cluster) {
        // TODO(SERVER-114573): Remove once v9.0 is last LTS and viewless timeseries upgrade/downgrade doesn't happen.
        // A downgrade may have been interrupted due to an index build (SERVER-119738), we must complete it before upgrading to latest.
        assert.commandWorkedOrFailedWithCode(
            db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
            // "10778001: Cannot downgrade featureCompatibilityVersion if a previous FCV upgrade stopped in the middle ..."
            // This error indicates that setFCV was interrupted during an upgrade rather than downgrade.
            // The next setFCV command will complete that upgrade and set the FCV to 'latest' for tests that run afterwards.
            10778001,
        );

        assert.commandWorked(
            db.adminCommand({
                setFeatureCompatibilityVersion: latestFCV,
                confirm: true,
            }),
        );
    };

    return {
        threadCount: 4,
        iterations: 100,
        startState: "init",
        states,
        transitions: uniformDistTransitions(states),
        setup,
        teardown,
    };
})();
