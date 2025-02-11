/**
 * Tests that the spilling stats are correctly returned in server status when $or is performed.
 *
 * @tags: [
 *     # Should not run on sharded suites due to use of serverStatus()
 *     assumes_unsharded_collection,
 *     do_not_wrap_aggregations_in_facets,
 *     assumes_read_preference_unchanged,
 *     assumes_read_concern_unchanged,
 *     assumes_against_mongod_not_mongos,
 *     does_not_support_repeated_reads,
 *     # Multi clients run concurrently and may modify the serverStatus metrics read in this test.
 *     multi_clients_incompatible,
 *     requires_fcv_81,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {checkSbeCompletelyDisabled} from "jstests/libs/query/sbe_util.js";

let t = db.getCollection("or_spilling_stats");
const memoryLimit = 128;  // Spill at 128B

t.drop();

assert.commandWorked(t.createIndexes([{a: 1}, {b: 1}]));

let docs = [];
for (let docId = 0; docId < 110; ++docId) {
    docs.push({_id: docId++, a: 1, b: 1});
}
assert.commandWorked(t.insert(docs));

function setOrStageMemoryLimit(memoryLimit) {
    const commandResArr = FixtureHelpers.runCommandOnEachPrimary({
        db: db.getSiblingDB("admin"),
        cmdObj: {
            setParameter: 1,
            internalOrStageMaxMemoryBytes: memoryLimit,
        }
    });
    assert.gt(commandResArr.length, 0, "Setting memory limit failed");
    assert.commandWorked(commandResArr[0]);
}

function getOrObj() {
    let serverStatusObj = db.serverStatus();
    assert(serverStatusObj && serverStatusObj.metrics && serverStatusObj.metrics.query &&
               serverStatusObj.metrics.query.or,
           `Server status is missing metrics: ${tojson(serverStatusObj)}`);
    let orObj = serverStatusObj.metrics.query.or;
    assert(orObj.spills, `Server status metrics.query.or does not have spills: ${tojson(orObj)}`);
    assert(orObj.spilledBytes,
           `Server status metrics.query.or does not have spilledBytes: ${tojson(orObj)}`);
    assert(orObj.spilledRecords,
           `Server status metrics.query.or does not have spilledRecords: ${tojson(orObj)}`);
    assert(orObj.spilledDataStorageSize,
           `Server status metrics.query.or does not have spilledDataStorageSize: ${tojson(orObj)}`);
    return orObj;
}

// Initial values of the metrics.
let orSpillMetricsInitial = getOrObj();

// Verifies that the query in classic shows evidence of spilling to disk.
function assertSpillingOccurredInClassic() {
    if (!checkSbeCompletelyDisabled(db)) {
        jsTestLog(
            "Skipping test because SBE is not disabled thus orStage is not used in the query");
        quit();
    }

    // Test that by default it does not spill.
    assert.eq(docs.length, t.countDocuments({$or: [{a: 1}, {b: 1}]}));
    let orObj = getOrObj();
    assert.eq(orObj.spills, orSpillMetricsInitial.spills);
    assert.eq(orObj.spilledBytes, orSpillMetricsInitial.spilledBytes);
    assert.eq(orObj.spilledRecords, orSpillMetricsInitial.spilledRecords);
    assert.eq(orObj.spilledDataStorageSize, orSpillMetricsInitial.spilledDataStorageSize);

    const oldMemoryLimit =
        assert.commandWorked(db.adminCommand({getParameter: 1, internalOrStageMaxMemoryBytes: 1}))
            .internalOrStageMaxMemoryBytes;

    // Test that it spills when the memory threshold is low.
    try {
        setOrStageMemoryLimit(memoryLimit);
        assert.eq(docs.length, t.countDocuments({$or: [{a: 1}, {b: 1}]}));
        orObj = getOrObj();
        assert.gt(orObj.spills, orSpillMetricsInitial.spills);
        assert.gt(orObj.spilledBytes, orSpillMetricsInitial.spilledBytes);
        assert.gt(orObj.spilledRecords, orSpillMetricsInitial.spilledRecords);
        assert.gt(orObj.spilledDataStorageSize, orSpillMetricsInitial.spilledDataStorageSize);

    } finally {
        setOrStageMemoryLimit(oldMemoryLimit);
    }
}

assertSpillingOccurredInClassic();
