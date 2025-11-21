/**
 * Tests explaining write operations on the raw buckets of a timeseries collection.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {
    getTimeseriesCollForRawOps,
    kIsRawOperationSupported,
    kRawOperationSpec,
} from "jstests/core/libs/raw_operation_utils.js";
import {getPlanStage} from "jstests/libs/query/analyze_plan.js";
import {
    areViewlessTimeseriesEnabled,
    getTimeseriesCollForDDLOps,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {isFCVlt} from "jstests/libs/feature_compatibility_version.js";

const coll = db[jsTestName()];

const timeField = "t";
const metaField = "m";
const time = new Date("2024-01-01T00:00:00Z");

coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));

assert.commandWorked(
    coll.insert([
        {[timeField]: time, [metaField]: 1, a: "a"},
        {[timeField]: time, [metaField]: 2, a: "b"},
        {[timeField]: time, [metaField]: 2, a: "c"},
    ]),
);

const assertExplain = function (commandResult, commandName) {
    assert(commandResult.ok);

    let targetColl = (() => {
        if (
            commandResult.command.findAndModify &&
            FixtureHelpers.isTracked(getTimeseriesCollForDDLOps(db, coll)) &&
            !areViewlessTimeseriesEnabled(db)
        ) {
            // In sharded clusters for findAndModify over legacy tracked timeseries we convert the namespace on the router and we send the command
            // with translated namespace to the shard,
            // thus we expect explain to report the command targeting system.buckets internal namespace.
            return getTimeseriesCollForDDLOps(db, coll);
        }
        return getTimeseriesCollForRawOps(coll);
    })();

    if (commandResult.command.bulkWrite) {
        assert.eq(
            commandResult.command.nsInfo.length,
            1,
            `Expected 1 namespace in explain command but got ${commandResult.command.nsInfo.length}`,
        );
        assert.eq(
            commandResult.command.nsInfo[0].ns,
            targetColl.getFullName(),
            `Expected command namespace to be ${tojson(targetColl.getFullName())} but got ${tojson(
                commandResult.command.nsInfo[0].ns,
            )}`,
        );
    } else {
        if (commandResult.command.findAndModify && isFCVlt(db.getMongo(), "8.3")) {
            // In versions 8.2 findAndModify explain return the main namespace instead of the system.buckets
            // TODO SERVER-114161 enable the check once the fix have been backported to previous versions
            jsTest.log(
                "Skipping namespace check for findAndModify explain output since FCV is less then 8.3 (BACKPORT-26389)",
            );
        } else if (
            commandResult.command.findAndModify &&
            !areViewlessTimeseriesEnabled(db) &&
            TestData.runningWithBalancer &&
            FixtureHelpers.isTracked(getTimeseriesCollForDDLOps(db, coll)) &&
            !FixtureHelpers.isSharded(getTimeseriesCollForDDLOps(db, coll))
        ) {
            // If the collection is tracked or untracked findAndModify explain returns either the buckets or main timeseries namespace
            // In suites with enabled balancer the collection could randomly became tracked.
            jsTest.log(
                "Skipping namespace check for findAndModify explain output in suites with random balancer enabled since we don't know if the collection was tracked or not when the command was executed",
            );
        } else {
            jsTest.log(`commandRes = ${tojson(commandResult)}`);
            assert.eq(
                commandResult.command[commandName],
                targetColl.getName(),
                `Expected command namespace to be ${tojson(targetColl.getName())} but got ${tojson(
                    commandResult.command[commandName],
                )}`,
            );
        }
    }
    assert(kIsRawOperationSupported === (commandResult.command.rawData ?? false));
    assert.isnull(getPlanStage(commandResult, "TS_MODIFY")),
        "Expected not to find TS_MODIFY stage " + tojson(commandResult);
};

assertExplain(
    getTimeseriesCollForRawOps(coll)
        .explain()
        .findAndModify({
            query: {"control.count": 2},
            update: {$set: {meta: "3"}},
            ...kRawOperationSpec,
        }),
    "findAndModify",
);
assertExplain(getTimeseriesCollForRawOps(coll).explain().remove({"control.count": 2}, kRawOperationSpec), "delete");
assertExplain(
    getTimeseriesCollForRawOps(coll)
        .explain()
        .update({"control.count": 1}, {$set: {meta: "3"}}, kRawOperationSpec),
    "update",
);

// Additionally run explains that issue a cluster write without a shard key in a sharded environment
// to test that path.
assertExplain(
    getTimeseriesCollForRawOps(coll)
        .explain()
        .remove({"control.count": 2}, {...kRawOperationSpec, justOne: true}),
    "delete",
);
assertExplain(
    getTimeseriesCollForRawOps(coll)
        .explain()
        .update({"_id": 1}, {$set: {meta: "3"}}, kRawOperationSpec),
    "update",
);
