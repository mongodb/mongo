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
    // change stream identified by 'comment'. Reused for both the positive (lookup landed here) and,
    // in the follow-up that enables the optimized local lookup (SERVER-129059), the negative (lookup
    // did NOT route to a given node) assertions.
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

    it("targets the tagged secondary and follows a reconfig, blocking on afterClusterTime", function () {
        const ns = mongosColl.getFullName();

        // Make sure reads with read preference tag 'closestSecondary' go to the tagged secondary.
        const closestSecondary = rst.nodes[1];
        const closestSecondaryDB = closestSecondary.getDB(mongosDB.getName());
        assert.commandWorked(closestSecondaryDB.setProfilingLevel(2));

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

        // Test that the update lookup goes to the secondary as well.
        // TODO SERVER-129059 When the optimized local lookup is enabled for sharded clusters, the
        // post-image is read locally on the stream's own node rather than routed to the tagged
        // secondary; this routed-lookup assertion (and the lagged-secondary scenario below) becomes
        // a "did NOT route" assertion via routedUpdateLookupFilter + profilerHasZeroMatchingEntries.
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: closestSecondaryDB,
            filter: routedUpdateLookupFilter(ns, changeStreamComment, mongosColl.getName(), {
                "command.pipeline.0.$match._id": 1,
            }),
            errorMsgFilter: {ns: ns},
            errorMsgProj: {ns: 1, op: 1, command: 1},
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

        // Test that the change stream continues on the original host, but the update lookup now
        // targets the new, lagged secondary. Even though it's lagged, the lookup should use
        // 'afterClusterTime' to ensure it does not return until the node can see the change it's
        // looking up.
        stopServerReplication(newClosestSecondary);
        assert.commandWorked(mongosColl.update({_id: 1}, {$set: {updatedCount: 2}}));

        // Since we stopped replication, we expect the update lookup to block indefinitely until we
        // resume replication, so we resume replication in a parallel shell while this thread is
        // blocked getting the next change from the stream.
        const noConnect = true; // This shell creates its own connection to the host.
        const joinResumeReplicationShell = startParallelShell(
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
                                   // Note the namespace here happens to be database.$cmd, because
                                   // we're blocked waiting for the read concern, which happens
                                   // before we get to the command processing level and adjust the
                                   // currentOp namespace to include the collection name.
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
        assert.soon(() => changeStream.hasNext());
        latestChange = changeStream.next();
        assert.eq(latestChange.operationType, "update");
        assert.docEq({_id: 1, updatedCount: 2}, latestChange.fullDocument);
        joinResumeReplicationShell();

        // Test that the update lookup goes to the new closest secondary.
        profilerHasSingleMatchingEntryOrThrow({
            profileDB: newClosestSecondaryDB,
            filter: routedUpdateLookupFilter(ns, changeStreamComment, mongosColl.getName()),
        });

        changeStream.close();
    });
});
