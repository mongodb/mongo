/**
 * Test that a replica set can be started where every node has a secondary port and
 * an INTERNAL split horizon that maps to the secondary port. Each node's INTERNAL
 * horizon uses a distinct hostname that resolves to 127.0.0.1 (local.10gen.cc,
 * lvh.me, localtest.me) paired with that node's secondary port.
 *
 * TLS is enabled with the same certificates used by sni_name_advertisement.js.
 *
 * @tags: [
 *   requires_replication,
 *   # The secondary port is based on ASIO, so gRPC testing is excluded
 *   grpc_incompatible,
 * ]
 */

if (_isWindows()) {
    quit();
}

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {checkLog} from "src/mongo/shell/check_log.js";
import {describe, it, before, after} from "jstests/libs/mochalite.js";

const pemKeyFile = "jstests/libs/server.pem";
const caFile = "jstests/libs/ca.pem";

const internalHostnames = ["local.10gen.cc", "lvh.me", "localtest.me"];
const numNodes = internalHostnames.length;

const secondaryPorts = [];
for (let i = 0; i < numNodes; i++) {
    secondaryPorts.push(allocatePort());
}

const nodeOptions = [];
for (let i = 0; i < numNodes; i++) {
    nodeOptions.push({
        secondaryPort: secondaryPorts[i],
        rsConfig: {
            horizons: {
                INTERNAL: `${internalHostnames[i]}:${secondaryPorts[i]}`,
            },
        },
    });
}

const dbName = jsTestName();
const collName = "testcoll";

describe("secondary port with INTERNAL split horizon", function () {
    before(function () {
        this.rst = new ReplSetTest({
            nodes: nodeOptions,
            nodeOptions: {
                tlsCertificateKeyFile: pemKeyFile,
                tlsCAFile: caFile,
                tlsMode: "preferTLS",
                tlsAllowInvalidCertificates: "",
            },
        });
        this.rst.startSet();
        this.rst.initiate();

        this.primary = this.rst.getPrimary();
        this.primaryId = this.rst.getNodeId(this.primary);
    });

    after(function () {
        this.rst.stopSet();
    });

    it("should have INTERNAL horizon configured on every node", function () {
        const config = assert.commandWorked(this.primary.adminCommand({replSetGetConfig: 1})).config;
        for (let i = 0; i < numNodes; i++) {
            const horizons = config.members[i].horizons;
            assert(horizons.hasOwnProperty("INTERNAL"), `Node ${i} should have an INTERNAL horizon`);
            assert.eq(
                horizons.INTERNAL,
                `${internalHostnames[i]}:${secondaryPorts[i]}`,
                `Node ${i} INTERNAL horizon mismatch`,
            );
        }
    });

    it("should support writes via the replica set connection string", function () {
        const conn = new Mongo(this.rst.getURL());
        const coll = conn.getDB(dbName).getCollection(collName);
        assert.commandWorked(coll.insert({_id: 1, src: "rsConn"}));
        assert.eq(coll.findOne({_id: 1}).src, "rsConn");

        this.rst.awaitReplication();
    });

    it("should return the default horizon via hello on the main port", function () {
        const config = assert.commandWorked(this.primary.adminCommand({replSetGetConfig: 1})).config;
        const expectedDefaultHosts = config.members.map((m) => m.host);
        const expectedMe = config.members[this.primaryId].host;

        const conn = new Mongo(`localhost:${this.primary.port}`);
        const helloResult = assert.commandWorked(conn.getDB("admin").runCommand({hello: 1}));

        assert.eq(
            helloResult.me,
            expectedMe,
            "hello 'me' on main port should match the member's host in the replica set config",
        );

        assert.sameMembers(
            helloResult.hosts,
            expectedDefaultHosts,
            "hello 'hosts' on main port should match the hosts in the replica set config",
        );

        conn.close();
    });

    it("should return the INTERNAL horizon via the secondary port with TLS", function () {
        const primarySecondaryPort = secondaryPorts[this.primaryId];
        const primaryInternalHostname = internalHostnames[this.primaryId];

        const conn = new Mongo(`${primaryInternalHostname}:${primarySecondaryPort}`, undefined, {
            tls: {
                certificateKeyFile: pemKeyFile,
                CAFile: caFile,
                allowInvalidCertificates: true,
            },
        });
        const db = conn.getDB("admin");

        const sniResult = assert.commandWorked(db.runCommand({whatsmysni: 1}));
        assert.eq(sniResult.sni, primaryInternalHostname, "SNI should match the INTERNAL horizon hostname");

        const helloResult = assert.commandWorked(db.runCommand({hello: 1}));

        const expectedMe = `${primaryInternalHostname}:${primarySecondaryPort}`;
        assert.eq(helloResult.me, expectedMe, "hello 'me' should match the primary's INTERNAL horizon");

        const expectedHosts = internalHostnames.map((h, i) => `${h}:${secondaryPorts[i]}`);
        assert.sameMembers(
            helloResult.hosts,
            expectedHosts,
            "hello 'hosts' should list all INTERNAL horizon addresses",
        );

        conn.close();
    });

    it("should support reads and writes via the secondary port replica set URL", function () {
        const tlsOptions = {
            tls: {
                certificateKeyFile: pemKeyFile,
                CAFile: caFile,
                allowInvalidCertificates: true,
            },
        };

        const secondaryPortHosts = internalHostnames.map((h, i) => `${h}:${secondaryPorts[i]}`).join(",");
        const rsName = this.rst.name;
        const rsURL = `${rsName}/${secondaryPortHosts}`;

        const conn = new Mongo(rsURL, undefined, tlsOptions);
        const coll = conn.getDB(dbName).getCollection(collName);

        assert.commandWorked(coll.insert({_id: "secondaryPort", src: "secondaryPortConn"}));

        const findResult = coll.findOne({_id: "secondaryPort"});
        assert.neq(findResult, null, "should be able to read the document written via the secondary port");
        assert.eq(findResult.src, "secondaryPortConn");

        conn.close();
    });

    it("should log a connection accepted on the secondary port", function () {
        const logs = checkLog.getGlobalLog(this.primary);
        const connectionAccepted = logs.some((line) => {
            return line.includes('"id":22943');
        });
        assert(connectionAccepted, "Expected at least one 'Connection accepted' (log 22943) entry in the server log");
    });
});
