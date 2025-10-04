/**
 * Tests that a no-op setFeatureCompatibilityVersion request still waits for write concern.
 *
 * This tests behavior on both latest and last-lts FCVs.
 * @tags: [multiversion_incompatible]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {assertWriteConcernError} from "jstests/libs/write_concern_util.js";
import {reconfig} from "jstests/replsets/rslib.js";

// Start a two node replica set and set its FCV to the given version, then take down one
// node so majority write concern can no longer be satisfied and verify that a noop setFCV
// request times out waiting for majority write concern.
function testFCVNoop(targetVersion) {
    jsTestLog("Testing setFeatureCompatibilityVersion with targetVersion: " + targetVersion);

    const replTest = new ReplSetTest({
        nodes: [{}, {rsConfig: {priority: 0}}],
    });
    replTest.startSet();
    replTest.initiate();

    const primary = replTest.getPrimary();
    assert.eq(primary, replTest.nodes[0]);
    // The default WC is majority and this test can't satisfy majority writes.
    assert.commandWorked(
        primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
    );

    // Set the FCV to the given target version, to ensure calling setFCV below is a no-op.
    assert.commandWorkedIgnoringWriteConcernErrors(
        primary.adminCommand({setFeatureCompatibilityVersion: targetVersion, confirm: true}),
    );

    // Stop one node to force commands with "majority" write concern to time out. First increase
    // the election timeout to prevent the primary from stepping down before the test is over.
    let conf = replTest.getReplSetConfigFromNode();
    conf.settings = {
        electionTimeoutMillis: 1000 * 60 * 10,
    };
    conf.version += 1;
    reconfig(replTest, conf);

    replTest.stop(1);

    // Insert a document to ensure there is a last optime.
    assert.commandWorked(primary.getDB("test").foo.insert({x: 1}));

    // We run the command on a different connection. If the the command were run on the same
    // connection, then the client last op for the noop write would be the last op of the
    // previous setFCV call. By using a fresh connection the client last op begins as null. This
    // test explicitly tests that write concern for noop writes works when the client last op
    // has not already been set by a duplicate operation.
    const shell2 = new Mongo(primary.host);

    // Use w:1 to verify setFCV internally waits for at least write concern majority, and use a
    // small wtimeout to verify it is propagated into the internal waitForWriteConcern and will
    // allow the command to timeout.
    const res = shell2.adminCommand({
        setFeatureCompatibilityVersion: targetVersion,
        confirm: true,
        writeConcern: {w: 1, wtimeout: 1000},
    });

    try {
        // Verify the command receives a write concern error. If we don't wait for write concern
        // on noop writes then we won't get a write concern error.
        assertWriteConcernError(res);
        assert.commandWorkedIgnoringWriteConcernErrors(res);
    } catch (e) {
        printjson(res);
        throw e;
    }

    replTest.stopSet();
}

testFCVNoop(lastLTSFCV);
testFCVNoop(latestFCV);
