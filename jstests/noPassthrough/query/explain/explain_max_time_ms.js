/**
 * Tests the explain command with the maxTimeMS option.
 */
import {runWithFailpoint} from "jstests/libs/query/command_diagnostic_utils.js";

const standalone = MongoRunner.runMongod();
assert.neq(null, standalone, "mongod was unable to start up");

const dbName = "test";
const db = standalone.getDB(dbName);
const collName = "explain_max_time_ms";
const coll = db.getCollection(collName);

const destCollName = "explain_max_time_ms_dest";
const mapFn = function () {
    emit(this.i, this.j);
};
const reduceFn = function (key, values) {
    return Array.sum(values);
};

coll.drop();
assert.commandWorked(db.createCollection(collName));

assert.commandWorked(coll.insert({i: 1, j: 1}));
assert.commandWorked(coll.insert({i: 2, j: 1}));
assert.commandWorked(coll.insert({i: 2, j: 2}));

// The shell explain helpers hoist a nested maxTimeMS up to the top level, so they would not exercise
// the path where maxTimeMS lives inside the explained command. Issue explain via raw db.runCommand
// to verify the server honors maxTimeMS in both cases across a range of explained commands.
const explainedCommands = [
    {find: collName, filter: {i: 1}},
    {aggregate: collName, pipeline: [{$match: {i: 1}}], cursor: {}},
    {count: collName, query: {i: 1}},
    {distinct: collName, key: "i"},
    {findAndModify: collName, update: {$inc: {j: 1}}},
    {mapReduce: collName, map: mapFn, reduce: reduceFn, out: destCollName},
];

// With the fail point on, any operation that has a deadline set times out immediately.
runWithFailpoint(db, "maxTimeAlwaysTimeOut", {}, () => {
    for (const verbosity of ["executionStats", "allPlansExecution"]) {
        for (const explained of explainedCommands) {
            // A maxTimeMS nested inside the explained command times out.
            assert.commandFailedWithCode(
                db.runCommand({explain: {...explained, maxTimeMS: 1}, verbosity}),
                ErrorCodes.MaxTimeMSExpired,
            );
            // A top-level maxTimeMS on the explain command times out.
            assert.commandFailedWithCode(
                db.runCommand({explain: explained, verbosity, maxTimeMS: 1}),
                ErrorCodes.MaxTimeMSExpired,
            );
        }
    }
});

const verbosity = "executionStats";

// With no fail point and no maxTimeMS, or a nested maxTimeMS that is not exceeded, explain succeeds.
assert.commandWorked(db.runCommand({explain: {find: collName, filter: {i: 1}}, verbosity}));
assert.commandWorked(
    db.runCommand({explain: {find: collName, filter: {i: 1}, maxTimeMS: 600000}, verbosity}),
);

// When both are provided, the smaller of the two maxTimeMS values should be honored. Block the explain long enough that only the
// smaller deadline is exceeded: the larger value on its own would not time out, so a timeout shows
// the smaller value was the one applied.
const smallMaxTimeMS = 1;
const largeMaxTimeMS = 600000;
runWithFailpoint(
    db,
    "failCommand",
    {failCommands: ["explain"], blockConnection: true, blockTimeMS: 1000},
    () => {
        // The top-level value is the smaller one.
        assert.commandFailedWithCode(
            db.runCommand({
                explain: {find: collName, filter: {i: 1}, maxTimeMS: largeMaxTimeMS},
                verbosity,
                maxTimeMS: smallMaxTimeMS,
            }),
            ErrorCodes.MaxTimeMSExpired,
        );
        // The nested value is the smaller one.
        assert.commandFailedWithCode(
            db.runCommand({
                explain: {find: collName, filter: {i: 1}, maxTimeMS: smallMaxTimeMS},
                verbosity,
                maxTimeMS: largeMaxTimeMS,
            }),
            ErrorCodes.MaxTimeMSExpired,
        );
    },
);

MongoRunner.stopMongod(standalone);
