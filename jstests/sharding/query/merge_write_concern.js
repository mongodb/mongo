// Tests that $merge respects the writeConcern set on the original aggregation command.
(function() {
"use strict";

load("jstests/aggregation/extras/merge_helpers.js");  // For withEachMergeMode.

const st = new ShardingTest({shards: 2, rs: {nodes: 3}});

const mongosDB = st.s0.getDB("merge_write_concern");
const source = mongosDB["source"];
const target = mongosDB["target"];
const shard0 = st.rs0;
const shard1 = st.rs1;

// Enable sharding on the test DB and ensure its primary is shard0.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
st.ensurePrimaryShard(mongosDB.getName(), st.shard0.shardName);

function testWriteConcernError(rs) {
    // Split the target collection at {_id: 10} so that there'll be doc $merge-ed to both shards.
    if (FixtureHelpers.isSharded(target)) {
        mongosDB.adminCommand({split: target.getFullName(), middle: {_id: 10}});
    }

    // Make sure that there are only 2 nodes up so w:3 writes will always time out.
    const stoppedSecondary = rs.getSecondary();
    rs.stop(stoppedSecondary);

    // Test that $merge correctly returns a WC error.
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        // When either mode is "fail", a different error rather than WC error is thrown.
        if (whenMatchedMode === "fail" || whenNotMatchedMode === "fail") {
            return;
        }
        const res = mongosDB.runCommand({
            aggregate: "source",
            pipeline: [{
                $merge: {
                    into: "target",
                    whenMatched: whenMatchedMode,
                    whenNotMatched: whenNotMatchedMode
                }
            }],
            writeConcern: {w: 3, wtimeout: 100},
            cursor: {},
        });

        jsTestLog("Testing Mode: " + tojson(whenMatchedMode) + tojson(whenNotMatchedMode));
        jsTestLog("Target collection after $merge: " + tojson(target.find().toArray()));

        let oplogEntries = shard0.getPrimary()
                               .getDB("local")
                               .oplog.rs.find({"ns": {$regex: "merge_write_concern.*"}})
                               .toArray();
        jsTestLog("Shard0 oplog entries: " + tojson(oplogEntries));
        oplogEntries = shard1.getPrimary()
                           .getDB("local")
                           .oplog.rs.find({"ns": {$regex: "merge_write_concern.*"}})
                           .toArray();
        jsTestLog("Shard1 oplog entries: " + tojson(oplogEntries));

        // $merge writeConcern errors are handled differently from normal writeConcern
        // errors. Rather than returing ok:1 and a WriteConcernError, the entire operation
        // fails.
        assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
        assert.commandWorked(target.remove({}));
    });

    // Restart the stopped node and verify that the $merge's now pass.
    rs.restart(rs.getSecondary());
    rs.awaitReplication();
    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        // Skip the combination of merge modes which will fail depending on the contents of the
        // source and target collection, as this will cause the assertion below to trip.
        if (whenNotMatchedMode == "fail")
            return;

        const res = mongosDB.runCommand({
            aggregate: "source",
            pipeline: [{
                $merge: {
                    into: "target",
                    whenMatched: whenMatchedMode,
                    whenNotMatched: whenNotMatchedMode
                }
            }],
            writeConcern: {w: 3},
            cursor: {},
        });

        // Ensure that the write concern is satisfied within a reasonable amount of time. This
        // prevents the test from hanging if for some reason the write concern can't be
        // satisfied.
        assert.soon(() => assert.commandWorked(res), "writeConcern was not satisfied");
        assert.commandWorked(target.remove({}));
    });
}

// Test that when both collections are unsharded, all writes are directed to the primary shard.
assert.commandWorked(source.insert([{_id: -1}, {_id: 0}, {_id: 1}, {_id: 2}]));
testWriteConcernError(shard0);

// Shard the source collection and continue to expect writes to the primary shard.
st.shardColl(source, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());
testWriteConcernError(shard0);

// Shard the target collection, however make sure that all writes go to the primary shard by
// splitting the collection at {_id: 10} and keeping all values in the same chunk.
st.shardColl(target, {_id: 1}, {_id: 10}, {_id: 10}, mongosDB.getName());
assert.eq(FixtureHelpers.isSharded(target), true);
testWriteConcernError(shard0);

// Write a few documents to the source collection which will be $merge-ed to the second shard.
assert.commandWorked(source.insert([{_id: 11}, {_id: 12}, {_id: 13}]));

// Verify that either shard can produce a WriteConcernError since writes are going to both.
testWriteConcernError(shard0);
testWriteConcernError(shard1);

// Verify that either shard can produce a WriteConcernError when the CatalogCache is empty.
[shard0, shard1].forEach(function(shard) {
    shard.nodes.forEach(function(node) {
        assert.commandWorked(node.adminCommand({flushRouterConfig: target.getFullName()}));
    });
});
testWriteConcernError(shard0);
testWriteConcernError(shard1);

st.stop();
}());
