/**
 * Tests that replSetGetStatus responses include the last applied and durable wall times for other
 * members.
 *
 * @tags: [multiversion_incompatible]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";

// We use GTE to account for the possibility of other writes in the system (e.g. HMAC).
// Comparison is GTE by default, GT if 'strict' is specified.
function checkWallTimes(primary, greaterMemberIndex, lesserMemberIndex, strict = false) {
    const ReduceMajorityWriteLatency =
        FeatureFlagUtil.isPresentAndEnabled(primary, "ReduceMajorityWriteLatency");
    assert.soonNoExcept(function() {
        let res = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
        assert(res.members, () => tojson(res));

        const greater = res.members[greaterMemberIndex];
        assert(greater, () => tojson(res));
        const greaterApplied = greater.lastAppliedWallTime;
        const greaterDurable = greater.lastAppliedWallTime;
        const greaterWritten = (ReduceMajorityWriteLatency) ? greater.lastWrittenWallTime : null;
        assert(greaterApplied, () => tojson(res));
        assert(greaterDurable, () => tojson(res));
        // If ReduceMajorityWriteLatency is set, greaterWritten will not be null, so
        // it'll be truthy.
        if (greaterWritten) {
            assert(greaterWritten, () => tojson(res));
        }

        const lesser = res.members[lesserMemberIndex];
        assert(lesser, () => tojson(res));
        const lesserApplied = lesser.lastAppliedWallTime;
        const lesserDurable = lesser.lastDurableWallTime;
        const lesserWritten = (ReduceMajorityWriteLatency) ? lesser.lastWrittenWallTime : null;
        assert(lesserApplied, () => tojson(res));
        assert(lesserDurable, () => tojson(res));
        if (lesserWritten) {
            assert(lesserWritten, () => tojson(res));
        }

        if (!strict) {
            assert.gte(greaterApplied, lesserApplied, () => tojson(res));
            assert.gte(greaterDurable, lesserDurable, () => tojson(res));
            if (greaterWritten && lesserWritten) {
                assert.gte(greaterWritten, lesserWritten, () => tojson(res));
            }
        } else {
            assert.gt(greaterApplied, lesserApplied, () => tojson(res));
            assert.gt(greaterDurable, lesserDurable, () => tojson(res));
            if (greaterWritten && lesserWritten) {
                assert.gt(greaterWritten, lesserWritten, () => tojson(res));
            }
        }

        return true;
    });
}

const name = jsTestName();
const rst = new ReplSetTest({name: name, nodes: 3, settings: {chainingAllowed: false}});

rst.startSet();
rst.initiateWithHighElectionTimeout();
rst.awaitReplication();

const primary = rst.getPrimary();                                   // node 0
const [caughtUpSecondary, laggedSecondary] = rst.getSecondaries();  // nodes 1 and 2

const dbName = "testdb";
const collName = "testcoll";
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

jsTestLog("Creating test collection");
assert.commandWorked(primaryColl.insert({"one": 1}));
rst.awaitReplication();

checkWallTimes(primary, 0 /* greater */, 1 /* lesser */);
checkWallTimes(primary, 0 /* greater */, 2 /* lesser */);

jsTestLog("Stopping replication on secondary: " + laggedSecondary.host);
stopServerReplication(laggedSecondary);

jsTestLog("Adding more documents to collection");
assert.commandWorked(primaryColl.insert({"two": 2}, {writeConcern: {w: 1}}));
rst.awaitReplication(
    undefined /* timeout */, undefined /* secondaryOpTimeType */, [caughtUpSecondary]);

// Wall times of the lagged secondary should be strictly lesser.
checkWallTimes(primary, 0 /* greater */, 2 /* lesser */, true /* strict */);
checkWallTimes(primary, 1 /* greater */, 2 /* lesser */, true /* strict */);

jsTestLog("Letting lagged secondary catch up");
restartServerReplication(laggedSecondary);
rst.awaitReplication();

checkWallTimes(primary, 0 /* greater */, 1 /* lesser */);
checkWallTimes(primary, 0 /* greater */, 2 /* lesser */);

rst.stopSet();
