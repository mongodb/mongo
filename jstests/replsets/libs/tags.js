/**
 * Sets up a test for replica set tags sets.
 *
 * https://docs.mongodb.com/v3.0/tutorial/configure-replica-set-tag-sets/
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {reconfig} from "jstests/replsets/rslib.js";

export var TagsTest = function (options) {
    // Skip db hash check since this test leaves replset partitioned.
    TestData.skipCheckDBHashes = true;

    if (!(this instanceof TagsTest)) {
        return new TagsTest(options);
    }

    // Capture the 'this' reference
    let self = this;

    self.options = options;

    /**
     * Runs the test.
     */
    this.run = function () {
        var options = this.options;
        let nodes = options.nodes;
        let host = getHostName();
        let name = "tags";

        let replTest = new ReplSetTest({name: name, nodes: {n0: nodes[0]}, useBridge: true});
        replTest.startSet();
        replTest.initiate();

        // If provided in 'options', we set the featureCompatibilityVersion. We do this prior to
        // adding any other members to the replica set. This effectively allows us to emulate
        // upgrading some of our nodes to the latest version while performing write operations under
        // different network partition scenarios.
        if (options.setFeatureCompatibilityVersion) {
            assert.commandWorked(
                replTest.getPrimary().adminCommand({
                    setFeatureCompatibilityVersion: options.setFeatureCompatibilityVersion,
                    confirm: true,
                }),
            );
        }

        for (let i = 1; i < nodes.length; ++i) {
            replTest.add(nodes[i]);
        }

        const conns = replTest.nodes;
        nodes = replTest.nodeList();
        let port = replTest.ports;
        let nextVersion = replTest.getReplSetConfigFromNode().version + 1;
        const replSetConfig = {
            _id: name,
            protocolVersion: 1,
            members: [
                {
                    _id: 0,
                    host: nodes[0],
                    tags: {
                        server: "0",
                        dc: "ny",
                        ny: "1",
                        rack: "ny.rk1",
                    },
                },
                {
                    _id: 1,
                    host: nodes[1],
                    tags: {
                        server: "1",
                        dc: "ny",
                        ny: "2",
                        rack: "ny.rk1",
                    },
                },
                {
                    _id: 2,
                    host: nodes[2],
                    tags: {
                        server: "2",
                        dc: "ny",
                        ny: "3",
                        rack: "ny.rk2",
                        2: "this",
                    },
                },
                {
                    _id: 3,
                    host: nodes[3],
                    priority: 0,
                    tags: {
                        server: "3",
                        dc: "sf",
                        sf: "1",
                        rack: "sf.rk1",
                    },
                },
                {
                    _id: 4,
                    host: nodes[4],
                    priority: 0,
                    tags: {
                        server: "4",
                        dc: "sf",
                        sf: "2",
                        rack: "sf.rk2",
                    },
                },
            ],
            settings: {
                getLastErrorModes: {
                    "2 dc and 3 server": {
                        dc: 2,
                        server: 3,
                    },
                    "1 and 2": {
                        2: 1,
                        server: 1,
                    },
                    "2": {
                        2: 1,
                    },
                    "3 and 4": {
                        sf: 2,
                    },
                    "3 or 4": {
                        sf: 1,
                    },
                },
            },
            version: nextVersion,
        };

        reconfig(replTest, replSetConfig);

        assert.soonNoExcept(() => replTest.nodes[2].adminCommand({replSetStepUp: 1}).ok);
        replTest.waitForState(replTest.nodes[2], ReplSetTest.State.PRIMARY);
        replTest.awaitReplication();
        replTest.waitForAllNewlyAddedRemovals();

        // Create collection to guard against timeouts due to file allocation.
        assert.commandWorked(replTest.getPrimary().getDB("foo").createCollection("bar"));
        replTest.awaitReplication();

        // nodeId is the index of the node that we expect to see as primary.
        // expectedNodesAgreeOnPrimary is a set of nodes that should agree that 'nodeId' is the
        // primary.
        // expectedWritableNodesCount is the number of nodes we can expect to write to. Defaults to
        //     expectedNodesAgreeOnPrimary.length.
        let ensurePrimary = function (nodeId, expectedNodesAgreeOnPrimary, expectedWritableNodesCount) {
            expectedWritableNodesCount = expectedWritableNodesCount || expectedNodesAgreeOnPrimary.length;
            jsTestLog("ensurePrimary - Node " + nodeId + " (" + replTest.nodes[nodeId].host + ") should be primary.");

            // Wait until the desired node steps up as primary and can accept writes.
            assert.soonNoExcept(() => assert.commandWorked(replTest.nodes[nodeId].adminCommand({replSetStepUp: 1})));
            assert.soon(() => replTest.getPrimary() === replTest.nodes[nodeId]);
            let primary = replTest.nodes[nodeId];

            // Wait until nodes know about the new primary.
            replTest.awaitNodesAgreeOnPrimary(replTest.timeoutMS, expectedNodesAgreeOnPrimary, nodeId);
            jsTestLog(
                "ensurePrimary - Nodes " +
                    tojson(expectedNodesAgreeOnPrimary) +
                    " agree that " +
                    nodeId +
                    " (" +
                    replTest.nodes[nodeId].host +
                    ") should be primary.",
            );

            let writeConcern = {
                writeConcern: {w: expectedWritableNodesCount, wtimeout: replTest.timeoutMS},
            };
            assert.commandWorked(primary.getDB("foo").bar.insert({x: 100}, writeConcern));
            jsTestLog(
                "ensurePrimary - Successfully written a document to primary node (" +
                    replTest.nodes[nodeId].host +
                    ") using a write concern of w:" +
                    expectedWritableNodesCount,
            );
            return primary;
        };

        // Make sure node 2 becomes primary.
        let primary = ensurePrimary(2, replTest.nodes);

        jsTestLog("primary is now 2");
        let config = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
        jsTestLog("test configuration = " + tojson(config));

        jsTestLog("Setting up partitions: [0-1-2] [3] [4]");
        conns[0].disconnect(conns[3]);
        conns[0].disconnect(conns[4]);
        conns[1].disconnect(conns[3]);
        conns[1].disconnect(conns[4]);
        conns[2].disconnect(conns[3]);
        conns[2].disconnect(conns[4]);
        conns[3].disconnect(conns[4]);
        jsTestLog("Done setting up partitions");

        jsTestLog("partitions: nodes with each set of brackets [N1, N2, N3] form a complete network.");
        jsTestLog("partitions: [0-1-2] [3] [4] (only nodes 0 and 1 can replicate from primary node 2");

        let doc = {x: 1};

        // This timeout should be shorter in duration than the server parameter
        // maxSyncSourceLagSecs.
        // Some writes are expected to block for this 'timeout' duration before failing.
        // Depending on the order of heartbeats (containing last committed op time) received
        // by a node, it might hang up on its sync source. This may cause some of the write concern
        // tests to fail.
        let failTimeout = 15 * 1000;

        jsTestLog("test1");
        primary = ensurePrimary(2, replTest.nodes.slice(0, 3));

        jsTestLog("Non-existent write concern should be rejected.");
        options = {writeConcern: {w: "blahblah", wtimeout: ReplSetTest.kDefaultTimeoutMS}};
        assert.commandWorked(primary.getDB("foo").bar.insert(doc));
        let result = assert.writeError(primary.getDB("foo").bar.insert(doc, options));
        assert.neq(null, result.getWriteConcernError());
        assert.eq(
            ErrorCodes.UnknownReplWriteConcern,
            result.getWriteConcernError().code,
            tojson(result.getWriteConcernError()),
        );

        jsTestLog('Write concern "3 or 4" should fail - 3 and 4 are not connected to the primary.');
        var options = {writeConcern: {w: "3 or 4", wtimeout: failTimeout}};
        assert.commandWorked(primary.getDB("foo").bar.insert(doc));
        result = primary.getDB("foo").bar.insert(doc, options);
        assert.neq(null, result.getWriteConcernError());
        assert(result.getWriteConcernError().errInfo.wtimeout);

        conns[1].reconnect(conns[4]);
        jsTestLog("partitions: [0-1-2] [1-4] [3] " + "(all nodes besides node 3 can replicate from primary node 2)");
        primary = ensurePrimary(2, replTest.nodes.slice(0, 3), 4);

        jsTestLog(
            'Write concern "3 or 4" should work - 4 is now connected to the primary ' +
                primary.host +
                " via node 1 " +
                replTest.nodes[1].host,
        );
        options = {writeConcern: {w: "3 or 4", wtimeout: ReplSetTest.kDefaultTimeoutMS}};
        assert.commandWorked(primary.getDB("foo").bar.insert(doc));
        assert.commandWorked(primary.getDB("foo").bar.insert(doc, options));

        jsTestLog('Write concern "3 and 4" should fail - 3 is not connected to the primary.');
        options = {writeConcern: {w: "3 and 4", wtimeout: failTimeout}};
        assert.commandWorked(primary.getDB("foo").bar.insert(doc));
        result = assert.writeError(primary.getDB("foo").bar.insert(doc, options));
        assert.neq(null, result.getWriteConcernError());
        assert(result.getWriteConcernError().errInfo.wtimeout, tojson(result.getWriteConcernError()));

        conns[3].reconnect(conns[4]);
        jsTestLog("partitions: [0-1-2] [1-4] [3-4] " + "(all secondaries can replicate from primary node 2)");
        primary = ensurePrimary(2, replTest.nodes.slice(0, 3), replTest.nodes.length);

        jsTestLog('Write concern "3 and 4" should work - ' + "nodes 3 and 4 are connected to primary via node 1.");
        options = {writeConcern: {w: "3 and 4", wtimeout: ReplSetTest.kDefaultTimeoutMS}};
        assert.commandWorked(primary.getDB("foo").bar.insert(doc));
        assert.commandWorked(primary.getDB("foo").bar.insert(doc, options));

        jsTestLog('Write concern "2" - writes to primary only.');
        options = {writeConcern: {w: "2", wtimeout: 0}};
        assert.commandWorked(primary.getDB("foo").bar.insert(doc));
        assert.commandWorked(primary.getDB("foo").bar.insert(doc, options));

        jsTestLog('Write concern "1 and 2"');
        options = {writeConcern: {w: "1 and 2", wtimeout: 0}};
        assert.commandWorked(primary.getDB("foo").bar.insert(doc));
        assert.commandWorked(primary.getDB("foo").bar.insert(doc, options));

        jsTestLog('Write concern "2 dc and 3 server"');
        primary = ensurePrimary(2, replTest.nodes.slice(0, 3), replTest.nodes.length);
        options = {writeConcern: {w: "2 dc and 3 server", wtimeout: ReplSetTest.kDefaultTimeoutMS}};
        assert.commandWorked(primary.getDB("foo").bar.insert(doc));
        assert.commandWorked(primary.getDB("foo").bar.insert(doc, options));

        jsTestLog(
            "Bringing down current primary node 2 " +
                primary.host +
                " to allow node 1 " +
                replTest.nodes[1].host +
                " to become primary.",
        );

        conns[1].reconnect(conns[3]);
        conns[2].disconnect(conns[0]);
        conns[2].disconnect(conns[1]);
        jsTestLog(
            "partitions: [0-1] [2] [1-3-4] " +
                "(all secondaries except down node 2 can replicate from new primary node 1)",
        );

        // Node 1 should take over.
        jsTestLog(
            "1 must become primary here because otherwise the other members will take too " +
                "long timing out their old sync threads",
        );
        primary = ensurePrimary(1, replTest.nodes.slice(0, 2), 4);

        jsTestLog('Write concern "3 and 4" should still work with new primary node 1 ' + primary.host);
        options = {writeConcern: {w: "3 and 4", wtimeout: ReplSetTest.kDefaultTimeoutMS}};
        assert.commandWorked(primary.getDB("foo").bar.insert(doc));
        assert.commandWorked(primary.getDB("foo").bar.insert(doc, options));

        jsTestLog('Write concern "2" should fail because node 2 ' + replTest.nodes[2].host + " is down.");
        options = {writeConcern: {w: "2", wtimeout: failTimeout}};
        assert.commandWorked(primary.getDB("foo").bar.insert(doc));
        result = assert.writeError(primary.getDB("foo").bar.insert(doc, options));
        assert.neq(null, result.getWriteConcernError());
        assert(result.getWriteConcernError().errInfo.wtimeout);

        jsTestLog("Setting custom write concern via a cluster-wide write concern");
        assert.commandWorked(
            primary.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: "3 and 4", wtimeout: ReplSetTest.kDefaultTimeoutMS},
            }),
        );

        jsTestLog('Custom write concern "3 and 4" should work');
        assert.commandWorked(primary.getDB("foo").bar.insert(doc));

        replTest.stopSet();
    };
};
