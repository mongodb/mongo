// Tests that $out respects the writeConcern set on the original aggregation command.
(function() {
    "use strict";

    load("jstests/aggregation/extras/out_helpers.js");  // For withEachOutMode() and isSharded().

    const st = new ShardingTest({shards: 2, rs: {nodes: 3}, config: 1});

    const mongosDB = st.s0.getDB("out_write_concern");
    const source = mongosDB["source"];
    const target = mongosDB["target"];
    const shard0 = st.rs0;
    const shard1 = st.rs1;

    // Enable sharding on the test DB and ensure its primary is shard0.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.shard0.shardName);

    function testWriteConcernError(rs) {
        // Make sure that there are only 2 nodes up so w:3 writes will always time out.
        const stoppedSecondary = rs.getSecondary();
        rs.stop(stoppedSecondary);

        // Test that $out correctly returns a WC error.
        withEachOutMode((mode) => {
            // Skip mode "replaceCollection" if the target collection is sharded, as it is
            // unsupported. Also skip mode "replaceCollection" if the test is expecting a timeout
            // against the non-primary shard, since this mode writes to a temp collection on the
            // primary only.
            if (mode == "replaceCollection" && (rs != shard0 || FixtureHelpers.isSharded(target)))
                return;

            const res = mongosDB.runCommand({
                aggregate: "source",
                pipeline: [{$out: {to: "target", mode: mode}}],
                writeConcern: {w: 3, wtimeout: 100},
                cursor: {},
            });

            // $out writeConcern errors are handled differently from normal writeConcern
            // errors. Rather than returing ok:1 and a WriteConcernError, the entire operation
            // fails.
            assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
            assert.commandWorked(target.remove({}));
        });

        // Restart the stopped node and verify that the $out's now pass.
        rs.restart(rs.getSecondary());
        rs.awaitReplication();
        withEachOutMode((mode) => {
            // "replaceCollection" is banned when the target collection is sharded.
            if (mode == "replaceCollection" && FixtureHelpers.isSharded(target))
                return;

            const res = mongosDB.runCommand({
                aggregate: "source",
                pipeline: [{$out: {to: "target", mode: mode}}],
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

    // Write a few documents to the source collection which will be $out-ed to the second shard.
    assert.commandWorked(source.insert([{_id: 11}, {_id: 12}, {_id: 13}]));

    // Verify that either shard can produce a WriteConcernError since writes are going to both.
    testWriteConcernError(shard0);
    testWriteConcernError(shard1);

    st.stop();
}());
