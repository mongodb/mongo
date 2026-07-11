/**
 * Tests that a change stream's update lookup will use the appropriate read concern. In particular,
 * tests that the update lookup will return a version of the document at least as recent as the
 * change that we're doing the lookup for, and that change will be majority-committed.
 * @tags: [
 *   # This test expects that there's only one shard (so no config shard) that has all the replSet
 *   # tags set.
 *   config_shard_incompatible,
 *   requires_majority_read_concern,
 *   requires_profiling,
 *   # This test has some timing dependency causing failures when run with a non-streamable rsm
 *   # (e.g. sdam), because non-streamable rsm is generally slower to learn of new replica set info.
 *   requires_streamable_rsm,
 *   uses_change_streams,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    profilerHasAtLeastOneMatchingEntryOrThrow,
    profilerHasSingleMatchingEntryOrThrow,
    profilerHasZeroMatchingEntriesOrThrow,
} from "jstests/libs/profiler.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {stopServerReplication} from "jstests/libs/write_concern_util.js";
import {awaitRSClientHosts, reconfig} from "jstests/replsets/rslib.js";

describe("change stream update lookup read concern and targeting", function () {
    let rst;
    let st;
    let mongosDB;
    let mongosColl;

    // Builds a profiler filter that selects the separately-routed update-lookup 'aggregate' for the
    // change stream identified by 'comment'. Used both for the legacy positive (lookup landed here)
    // and the optimized negative (lookup did NOT route to a given node) assertions.
    function routedUpdateLookupFilter(ns, comment, collName, extra = {}) {
        // We need to filter out any profiler entries with a stale config - the first read on a
        // secondary with a readConcern specified is the first read that will enforce shard version.
        return Object.assign(
            {
                op: "command",
                ns: ns,
                "command.comment": comment,
                errCode: {$ne: ErrorCodes.StaleConfig},
                "command.aggregate": collName,
            },
            extra,
        );
    }

    // Asserts where the post-image update lookup ran. Legacy routes a separate 'aggregate' to the
    // read-preference-selected node, so we expect exactly one matching profiler entry there. The
    // optimized lookup reads locally on the stream's own node and never routes, so we expect none.
    // 'streamNodeIsRunningOptimizedUpdateLookup' reflects the *stream's* node, not 'profileDB' (which
    // may be a different node the legacy path would route to) -- that's what actually determines
    // whether the lookup stays local or routes elsewhere.
    function assertUpdateLookupTargeting({
        profileDB,
        ns,
        comment,
        collName,
        extra = {},
        streamNodeIsRunningOptimizedUpdateLookup,
    }) {
        const filter = routedUpdateLookupFilter(ns, comment, collName, extra);
        if (streamNodeIsRunningOptimizedUpdateLookup) {
            profilerHasZeroMatchingEntriesOrThrow({profileDB, filter});
        } else {
            profilerHasSingleMatchingEntryOrThrow({
                profileDB,
                filter,
                errorMsgFilter: {ns: ns},
                errorMsgProj: {ns: 1, op: 1, command: 1},
            });
        }
    }

    before(function () {
        // Configure a replica set to have nodes with specific tags - we will eventually add this as
        // part of a sharded cluster.
        const rsNodeOptions = {
            setParameter: {
                writePeriodicNoops: true,
                // Note we do not configure the periodic noop writes to be more frequent as we do to
                // speed up other change streams tests, since we provide an array of individually
                // configured nodes, in order to know which nodes have which tags. This requires a
                // step up command to happen, which requires all nodes to agree on an op time. With
                // the periodic noop writer at a high frequency, this can potentially never finish.
            },
            shardsvr: "",
        };

        // Note that we include {chainingAllowed: false} in the replica set settings, because this
        // test assumes that both secondaries sync from the primary. Without this setting, the
        // TopologyCoordinator would sometimes chain one of the secondaries off the other. The test
        // later disables replication on one secondary, but with chaining, that would effectively
        // disable replication on both secondaries, deadlocking the test.
        rst = new ReplSetTest({
            nodes: [
                {rsConfig: {priority: 1, tags: {tag: "primary"}}},
                {rsConfig: {priority: 0, tags: {tag: "closestSecondary"}}},
                {rsConfig: {priority: 0, tags: {tag: "fartherSecondary"}}},
            ],
            nodeOptions: rsNodeOptions,
            settings: {chainingAllowed: false},
        });

        rst.startSet();
        rst.initiate();
        rst.awaitSecondaryNodes();

        // Start the sharding test and add the replica set.
        st = new ShardingTest({manualAddShard: true});
        assert.commandWorked(st.s.adminCommand({addShard: rst.name + "/" + rst.getPrimary().host}));

        // The default WC is majority and stopServerReplication will prevent satisfying any majority
        // writes.
        assert.commandWorked(
            st.s.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: 1},
                writeConcern: {w: "majority"},
            }),
        );

        mongosDB = st.s0.getDB(jsTestName());
        mongosColl = mongosDB[jsTestName()];

        assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
        assert.commandWorked(
            mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}),
        );

        assert.commandWorked(mongosColl.insert({_id: 1}));
        rst.awaitReplication();
    });

    after(function () {
        st.stop();
        rst.stopSet();
    });

    it("resolves the update lookup across a reconfig that introduces a closer secondary", function () {
        const ns = mongosColl.getFullName();

        // Make sure reads with read preference tag 'closestSecondary' go to the tagged secondary.
        const closestSecondary = rst.nodes[1];
        const closestSecondaryDB = closestSecondary.getDB(mongosDB.getName());
        assert.commandWorked(closestSecondaryDB.setProfilingLevel(2));

        // The change stream we open below stays pinned to this node for the rest of the test, even
        // after the reconfig moves the 'closestSecondary' tag elsewhere. Checked here (not via the
        // router or globally), since in a multiversion cluster this node's binary determines whether
        // its own update lookups are optimized, independent of any other node's binary.
        const streamNodeIsRunningOptimizedUpdateLookup = FeatureFlagUtil.isPresentAndEnabled(
            closestSecondaryDB.getSiblingDB("admin"),
            "ChangeStreamOptimizedUpdateLookup",
        );

        // Do a read concern "local" read so that the secondary refreshes its metadata.
        mongosColl.find().readPref("secondary", [{tag: "closestSecondary"}]);

        // We expect the tag to ensure there is only one node to choose from, so the actual read
        // preference doesn't really matter - we use 'nearest' throughout.
        assert.eq(
            mongosColl
                .find()
                .readPref("nearest", [{tag: "closestSecondary"}])
                .comment("testing targeting")
                .itcount(),
            1,
        );
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: closestSecondaryDB,
            filter: {ns: ns, "command.comment": "testing targeting"},
        });

        const changeStreamComment = "change stream against closestSecondary";
        const changeStream = mongosColl.aggregate(
            [{$changeStream: {fullDocument: "updateLookup"}}],
            {
                comment: changeStreamComment,
                $readPreference: {mode: "nearest", tags: [{tag: "closestSecondary"}]},
            },
        );
        assert.commandWorked(mongosColl.update({_id: 1}, {$set: {updatedCount: 1}}));
        assert.soon(() => changeStream.hasNext());
        let latestChange = changeStream.next();
        assert.eq(latestChange.operationType, "update");
        assert.docEq({_id: 1, updatedCount: 1}, latestChange.fullDocument);

        // Test that the change stream itself goes to the secondary. There might be more than one if
        // we needed multiple getMores to retrieve the changes.
        // TODO SERVER-31650 We have to use 'originatingCommand' here and look for the getMore
        // because the initial aggregate will not show up.
        profilerHasAtLeastOneMatchingEntryOrThrow({
            profileDB: closestSecondaryDB,
            filter: {"originatingCommand.comment": changeStreamComment},
        });
        assertUpdateLookupTargeting({
            profileDB: closestSecondaryDB,
            ns,
            comment: changeStreamComment,
            collName: mongosColl.getName(),
            extra: {"command.pipeline.0.$match._id": 1},
            streamNodeIsRunningOptimizedUpdateLookup,
        });

        // Now add a new secondary which is "closer" (add the "closestSecondary" tag to that
        // secondary, and remove it from the old node with that tag) to force update lookups target a
        // different node than the change stream itself.
        let rsConfig = rst.getReplSetConfig();
        rsConfig.members[1].tags = {tag: "fartherSecondary"};
        rsConfig.members[2].tags = {tag: "closestSecondary"};
        rsConfig.version = rst.getReplSetConfigFromNode().version + 1;
        reconfig(rst, rsConfig);
        rst.awaitSecondaryNodes();
        const newClosestSecondary = rst.nodes[2];
        const newClosestSecondaryDB = newClosestSecondary.getDB(mongosDB.getName());
        const originalClosestSecondaryDB = closestSecondaryDB;

        // Wait for the mongos to acknowledge the new tags from our reconfig.
        awaitRSClientHosts(
            st.s,
            newClosestSecondary,
            {ok: true, secondary: true, tags: {tag: "closestSecondary"}},
            rst,
        );
        awaitRSClientHosts(
            st.s,
            originalClosestSecondaryDB.getMongo(),
            {ok: true, secondary: true, tags: {tag: "fartherSecondary"}},
            rst,
        );
        assert.commandWorked(newClosestSecondaryDB.setProfilingLevel(2));

        // Make sure new queries with read preference tag "closestSecondary" go to the new secondary.
        profilerHasZeroMatchingEntriesOrThrow({profileDB: newClosestSecondaryDB, filter: {}});
        assert.eq(
            mongosColl
                .find()
                .readPref("nearest", [{tag: "closestSecondary"}])
                .comment("testing targeting")
                .itcount(),
            1,
        );
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: newClosestSecondaryDB,
            filter: {ns: ns, "command.comment": "testing targeting"},
        });

        // The change stream continues on the original host. Legacy re-routes the next update lookup
        // to the new, lagged secondary, where it blocks on 'afterClusterTime' until the node sees the
        // change. The optimized lookup reads locally on the original host and never targets the new
        // node, so its lag is irrelevant and there is nothing to unblock.
        let joinResumeReplicationShell;
        if (!streamNodeIsRunningOptimizedUpdateLookup) {
            stopServerReplication(newClosestSecondary);
        }
        assert.commandWorked(mongosColl.update({_id: 1}, {$set: {updatedCount: 2}}));

        if (!streamNodeIsRunningOptimizedUpdateLookup) {
            // Since we stopped replication, we expect the update lookup to block indefinitely until
            // we resume replication, so we resume replication in a parallel shell while this thread
            // is blocked getting the next change from the stream.
            const noConnect = true; // This shell creates its own connection to the host.
            joinResumeReplicationShell = startParallelShell(
                `
                import {restartServerReplication} from "jstests/libs/write_concern_util.js";

                const pausedSecondary = new Mongo("${newClosestSecondary.host}");

                // Wait for the update lookup to appear in currentOp.
                const changeStreamDB = pausedSecondary.getDB("${mongosDB.getName()}");
                assert.soon(
                    function() {
                        return changeStreamDB
                                   .currentOp({
                                       op: "command",
                                       // Note the namespace here happens to be database.$cmd,
                                       // because we're blocked waiting for the read concern, which
                                       // happens before we get to the command processing level and
                                       // adjust the currentOp namespace to include the collection
                                       // name.
                                       ns: "${mongosDB.getName()}.$cmd",
                                       "command.comment": "${changeStreamComment}",
                                   })
                                   .inprog.length === 1;
                    },
                    () => "Failed to find update lookup in currentOp(): " +
                        tojson(changeStreamDB.currentOp().inprog));

                // Then restart replication - this should eventually unblock the lookup.
                restartServerReplication(pausedSecondary);`,
                undefined,
                noConnect,
            );
        }
        assert.soon(() => changeStream.hasNext());
        latestChange = changeStream.next();
        assert.eq(latestChange.operationType, "update");
        assert.docEq({_id: 1, updatedCount: 2}, latestChange.fullDocument);
        if (joinResumeReplicationShell) {
            joinResumeReplicationShell();
        }
        assertUpdateLookupTargeting({
            profileDB: newClosestSecondaryDB,
            ns,
            comment: changeStreamComment,
            collName: mongosColl.getName(),
            streamNodeIsRunningOptimizedUpdateLookup,
        });

        changeStream.close();
    });
});
