/**
 * Confirms that the aggregate command accepts writeConcern regardless of whether the pipeline
 * writes or is read-only.
 * @tags: [
 *  assumes_write_concern_unchanged,
 *  does_not_support_stepdowns,
 *  references_foreign_collection
 * ]
 */
const testDB = db.getSiblingDB("aggregation_accepts_write_concern");
assert.commandWorked(testDB.dropDatabase());
const collName = "test";

assert.commandWorked(
    testDB.runCommand({insert: collName, documents: [{_id: 1}], writeConcern: {w: "majority"}}));

// A read-only aggregation accepts writeConcern.
assert.commandWorked(testDB.runCommand({
    aggregate: collName,
    pipeline: [{$match: {_id: 1}}],
    cursor: {},
    writeConcern: {w: "majority"}
}));

// An aggregation pipeline that writes accepts writeConcern.
let attempt = 0;
let res;
assert.soon(
    () => {
        attempt++;
        res = testDB.runCommand({
            aggregate: collName,
            pipeline: [{$match: {_id: 1}}, {$out: collName + "_out"}],
            cursor: {},
            writeConcern: {w: "majority"}
        });
        if (res.ok) {
            return true;
        } else if (TestData.runningWithBalancer && res.code == ErrorCodes.QueryPlanKilled) {
            // sometimes the balancer will race with this agg and kill the query,
            // so just retry.
            print("Retrying aggregate() after being killed attempt " + attempt +
                  " res: " + tojson(res));
            return false;
        } else {
            doassert("command failed: " + tojson(res));
        }
    },
    () => "Timed out while retrying aggregate(), response: " + tojson(res),
);
