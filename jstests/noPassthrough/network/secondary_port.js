/**
 * Test that a mongod can listen on a secondary port in addition to the main port,
 * and that clients can connect and run commands on both.
 *
 * @tags: [
 *   requires_replication,
 *   # The secondary port is based on ASIO, so gRPC testing is excluded
 *   grpc_incompatible,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {describe, it, before, after} from "jstests/libs/mochalite.js";

const secondaryPort = allocatePort();

describe("secondary port on a single-node replica set", function () {
    before(function () {
        this.rst = new ReplSetTest({
            nodes: [{secondaryPort: secondaryPort}],
        });
        this.rst.startSet();
        this.rst.initiate();

        const primary = this.rst.getPrimary();
        this.mainPort = primary.port;
        this.host = primary.hostNoPort;
    });

    after(function () {
        this.rst.stopSet();
    });

    it("should respond to commands on the main port", function () {
        const mainConn = new Mongo(`${this.host}:${this.mainPort}`);
        const res = assert.commandWorked(mainConn.adminCommand({ping: 1}));
        assert.eq(res.ok, 1, "ping on main port should succeed");

        const helloRes = assert.commandWorked(mainConn.adminCommand({hello: 1}));
        assert(helloRes.isWritablePrimary, "node should be writable primary on main port");

        mainConn.close();
    });

    it("should respond to commands on the secondary port", function () {
        const secondaryConn = new Mongo(`${this.host}:${secondaryPort}`);
        const res = assert.commandWorked(secondaryConn.adminCommand({ping: 1}));
        assert.eq(res.ok, 1, "ping on secondary port should succeed");

        const helloRes = assert.commandWorked(secondaryConn.adminCommand({hello: 1}));
        assert(helloRes.isWritablePrimary, "node should be writable primary on secondary port");

        secondaryConn.close();
    });

    it("should make writes on the main port visible from the secondary port", function () {
        const dbName = jsTestName();
        const collName = "testcoll";

        const mainConn = new Mongo(`${this.host}:${this.mainPort}`);
        const mainDB = mainConn.getDB(dbName);
        assert.commandWorked(mainDB.getCollection(collName).insert({_id: 1, x: "fromMainPort"}));

        const secondaryConn = new Mongo(`${this.host}:${secondaryPort}`);
        const secondaryDB = secondaryConn.getDB(dbName);
        const doc = secondaryDB.getCollection(collName).findOne({_id: 1});
        assert.neq(doc, null, "document inserted via main port should be readable via secondary port");
        assert.eq(doc.x, "fromMainPort");

        mainConn.close();
        secondaryConn.close();
    });
});
