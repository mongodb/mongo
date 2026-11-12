/**
 * Tests that the 'validate' command with 'atClusterTime' rejects combination with repair.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 *   requires_replication,
 * ]
 */

import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = jsTestName();
const collName = jsTestName();

const docs = [{_id: 1}, {_id: 2}, {_id: 3}];

describe("Repair is not allowed for point-in-time validation on a replica set", () => {
    let rst, atClusterTime;

    before(function () {
        rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
        rst.startSet();
        rst.initiate();
        const primary = rst.getPrimary();

        assert.commandWorked(primary.getDB(dbName).createCollection(collName));
        const res = assert.commandWorked(
            primary.getDB(dbName).runCommand({
                insert: collName,
                documents: docs,
                writeConcern: {w: "majority"},
            }),
        );
        atClusterTime = res.operationTime;
        rst.awaitReplication();
        for (const node of rst.nodes) {
            assert.commandWorked(node.adminCommand({fsync: 1}));
        }
    });

    afterEach(function () {
        for (const node of rst.nodes) {
            assert.commandWorked(node.adminCommand({ping: 1}));
        }
    });

    after(function () {
        rst.stopSet();
    });

    it("rejects repair with atClusterTime options", () => {
        for (const node of rst.nodes) {
            assert.commandFailed(
                node.getDB(dbName).runCommand({validate: collName, repair: true, atClusterTime}),
            );
        }
    });

    it("rejects background with atClusterTime options", () => {
        for (const node of rst.nodes) {
            assert.commandFailed(
                node
                    .getDB(dbName)
                    .runCommand({validate: collName, background: true, atClusterTime}),
            );
        }
    });

    it("rejects fixMultikey with atClusterTime options", () => {
        for (const node of rst.nodes) {
            assert.commandFailed(
                node
                    .getDB(dbName)
                    .runCommand({validate: collName, fixMultikey: true, atClusterTime}),
            );
        }
    });
});

describe("Repair is not allowed for point-in-time validation on a standalone", () => {
    let conn;

    const atClusterTime = Timestamp(1, 1);

    beforeEach(function () {
        conn = MongoRunner.runMongod();
        assert.commandWorked(conn.getDB(dbName).createCollection(collName));
        assert.commandWorked(conn.getDB(dbName).getCollection(collName).insertMany(docs));
        assert.commandWorked(conn.adminCommand({fsync: 1}));
    });

    afterEach(function () {
        let crashed = true;
        try {
            assert.commandWorked(conn.adminCommand({ping: 1}));
            crashed = false;
        } finally {
            MongoRunner.stopMongod(
                conn,
                undefined,
                crashed ? {allowedExitCode: MongoRunner.EXIT_ABORT} : {},
            );
        }
    });

    it("rejects repair with atClusterTime options", () => {
        assert.commandFailed(
            conn.getDB(dbName).runCommand({validate: collName, repair: true, atClusterTime}),
        );
    });

    it("rejects background with atClusterTime options", () => {
        assert.commandFailed(
            conn.getDB(dbName).runCommand({validate: collName, background: true, atClusterTime}),
        );
    });

    it("rejects fixMultikey with atClusterTime options", () => {
        assert.commandFailed(
            conn.getDB(dbName).runCommand({validate: collName, fixMultikey: true, atClusterTime}),
        );
    });
});
