// This script checks for leaked cursors in the MongoDB system.
import {DiscoverTopology} from "jstests/libs/discover_topology.js";

function findIdleCursors(conn) {
    try {
        const systemIntrospectionFilter = {
            $nor: [
                // Commands:
                {"cursor.originatingCommand.listDatabases": {$exists: true}},
                {"cursor.originatingCommand.listCollections": {$exists: true}},
                {"cursor.originatingCommand.listIndexes": {$exists: true}},

                // Aggregation pipelines:
                {"cursor.originatingCommand.pipeline.0.$listCatalog": {$exists: true}},
                {"cursor.originatingCommand.pipeline.0.$indexStats": {$exists: true}},
                {"cursor.originatingCommand.pipeline.0.$collStats": {$exists: true}},
            ],
        };
        const systemCollections = ["local.oplog.rs", "config.system.sessions"];

        return conn
            .getDB("admin")
            .aggregate(
                [
                    {$currentOp: {localOps: true, idleCursors: true}},
                    {$match: {"command.comment": {$ne: "$currentOp"}}},
                    {$match: {"type": "idleCursor"}},
                    {$match: {"ns": {$nin: systemCollections}}},
                    {$match: systemIntrospectionFilter},
                ],
                {comment: "$currentOp"},
            )
            .toArray();
    } catch (error) {
        jsTest.log.info(`Failed to read currentOps for connection`, {conn, error});
        return [];
    }
}

function assertNoIdleCursors(conn) {
    let idleCursors = findIdleCursors(conn);
    jsTest.log.info("Idle cursor for connection", {conn, idleCursors});

    // Currently some tests leak idle cursors. In order to not fail those tests we kill the leaked cursors.
    if (TestData && TestData.shouldKillIdleCursors) {
        jsTest.log.info("Killing idle cursors for connection", {conn, idleCursors});
        idleCursors.forEach(function (idleCursor) {
            assert.commandWorked(
                conn.getDB("admin").runCommand({killCursors: idleCursor.ns, cursors: [idleCursor.cursor.cursorId]}),
            );
        });

        idleCursors = findIdleCursors(conn);
    }

    assert.eq(0, idleCursors.length, `Expected no idleCursors left over on connection ${conn}`);
}

(function () {
    const nodesToConnectTo = (function () {
        const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
        if (topology.mongos) {
            const shardConnStrings = Object.values(topology.shards).flatMap((shardInfo) => shardInfo.nodes);
            return topology.mongos.nodes.concat(shardConnStrings);
        }

        return [topology.nodes];
    })();
    jsTest.log.info("Nodes to check for leaked cursors", {nodesToConnectTo});
    nodesToConnectTo.map((connString) => new Mongo(connString)).forEach(assertNoIdleCursors);
})();
