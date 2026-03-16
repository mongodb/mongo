/**
 * Test that the net.secondaryPort configuration works correctly in a 3-node replica set where
 * only a subset of nodes have a secondary port configured. Validates that:
 *   - Reads and writes work through a replica set connection string on the main ports.
 *   - Direct connections to secondary ports support reads.
 *   - Writes via the secondary port of the primary succeed.
 *   - Writes via the secondary port of an RS secondary fail with NotWritablePrimary.
 *
 * @tags: [
 *   requires_replication,
 *   # The secondary port is based on ASIO, so gRPC testing is excluded
 *   grpc_incompatible,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {describe, it, before, after} from "jstests/libs/mochalite.js";

const secondaryPortForPrimary = allocatePort();
const secondaryPortForRSSecondary = allocatePort();
const dbName = jsTestName();
const collName = "testcoll";

describe("secondary port in a 3-node replica set", function () {
    before(function () {
        this.rst = new ReplSetTest({
            nodes: [
                {secondaryPort: secondaryPortForPrimary, rsConfig: {priority: 2}},
                {secondaryPort: secondaryPortForRSSecondary},
                {
                    /* no secondary port */
                },
            ],
        });
        this.rst.startSet();
        this.rst.initiate();

        const primary = this.rst.getPrimary();
        this.host = primary.hostNoPort;
        assert.eq(this.rst.getNodeId(primary), 0, "node 0 should be primary due to higher priority");
    });

    after(function () {
        this.rst.stopSet();
    });

    it("should support reads and writes via the replica set connection string and reads on secondary port", function () {
        const rsURL = this.rst.getURL();
        const conn = new Mongo(rsURL);
        const coll = conn.getDB(dbName).getCollection(collName);

        assert.commandWorked(coll.insert({_id: 1, src: "rsConn"}));
        assert.eq(coll.findOne({_id: 1}).src, "rsConn");

        conn.close();

        this.rst.awaitReplication();

        {
            const conn = new Mongo(`${this.host}:${secondaryPortForPrimary}`);
            const coll = conn.getDB(dbName).getCollection(collName);
            const doc = coll.findOne({_id: 1});
            assert.neq(doc, null, "document should be readable via primary's secondary port");
            assert.eq(doc.src, "rsConn");
            conn.close();
        }

        {
            const conn = new Mongo(`${this.host}:${secondaryPortForRSSecondary}`);
            conn.setSecondaryOk();
            assert.commandWorked(conn.adminCommand({ping: 1}));

            const doc = conn.getDB(dbName).getCollection(collName).findOne({_id: 1});
            assert.neq(doc, null, "document should be readable via RS secondary's secondary port");
            assert.eq(doc.src, "rsConn");

            conn.close();
        }
    });

    it("should allow writes via the primary's secondary port", function () {
        const conn = new Mongo(`${this.host}:${secondaryPortForPrimary}`);
        const coll = conn.getDB(dbName).getCollection(collName);

        assert.commandWorked(coll.insert({_id: 3, src: "primarySecondaryPort"}));
        assert.eq(coll.findOne({_id: 3}).src, "primarySecondaryPort");

        conn.close();
    });

    it("should reject writes via the RS secondary's secondary port with NotWritablePrimary", function () {
        const conn = new Mongo(`${this.host}:${secondaryPortForRSSecondary}`);
        const coll = conn.getDB(dbName).getCollection(collName);

        assert.commandFailedWithCode(
            coll.insert({_id: 3, src: "secondarySecondaryPort"}),
            ErrorCodes.NotWritablePrimary,
        );

        conn.close();
    });
});
