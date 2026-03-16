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

rst = new ReplSetTest({
    nodes: nodeOptions,
    nodeOptions: {
        tlsCertificateKeyFile: pemKeyFile,
        tlsCAFile: caFile,
        tlsMode: "preferTLS",
        tlsAllowInvalidCertificates: "",
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryId = rst.getNodeId(primary);

// should have INTERNAL horizon configured on every node
{
    const config = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
    for (let i = 0; i < numNodes; i++) {
        const horizons = config.members[i].horizons;
        assert(horizons.hasOwnProperty("INTERNAL"), `Node ${i} should have an INTERNAL horizon`);
        assert.eq(
            horizons.INTERNAL,
            `${internalHostnames[i]}:${secondaryPorts[i]}`,
            `Node ${i} INTERNAL horizon mismatch`,
        );
    }
}

// should support writes via the replica set connection string
{
    const conn = new Mongo(rst.getURL());
    const coll = conn.getDB(dbName).getCollection(collName);
    assert.commandWorked(coll.insert({_id: 1, src: "rsConn"}));
    assert.eq(coll.findOne({_id: 1}).src, "rsConn");

    rst.awaitReplication();
}

// should return the default horizon via hello on the main port
{
    const config = assert.commandWorked(primary.adminCommand({replSetGetConfig: 1})).config;
    const expectedDefaultHosts = config.members.map((m) => m.host);
    const expectedMe = config.members[primaryId].host;

    const conn = new Mongo(`localhost:${primary.port}`);
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
}

// should return the INTERNAL horizon via the secondary port with TLS
{
    const primarySecondaryPort = secondaryPorts[primaryId];
    const primaryInternalHostname = internalHostnames[primaryId];

    const expectedMe = `${primaryInternalHostname}:${primarySecondaryPort}`;
    const expectedHosts = internalHostnames.map((h, i) => `${h}:${secondaryPorts[i]}`);

    const evalScript = `
const adminDB = db.getSiblingDB("admin");
const sniResult = assert.commandWorked(adminDB.runCommand({whatsmysni: 1}));
assert.eq(
    sniResult.sni,
    ${tojson(primaryInternalHostname)},
    "SNI should match the INTERNAL horizon hostname");
const helloResult = assert.commandWorked(adminDB.runCommand({hello: 1}));
assert.eq(
    helloResult.me,
    ${tojson(expectedMe)},
    "hello 'me' should match the primary's INTERNAL horizon");
assert.sameMembers(
    helloResult.hosts,
    ${tojson(expectedHosts)},
    "hello 'hosts' should list all INTERNAL horizon addresses");
`;

    const res = runMongoProgram("mongo",
                                "--host",
                                primaryInternalHostname,
                                "--port",
                                primarySecondaryPort,
                                "--tls",
                                "--tlsAllowInvalidCertificates",
                                "--tlsCAFile",
                                caFile,
                                "--tlsCertificateKeyFile",
                                pemKeyFile,
                                "--eval",
                                evalScript);
    assert.eq(res, 0, "TLS secondary-port shell program should succeed");
}

// should support reads and writes via the secondary port replica set URL
{
    const secondaryPortHosts =
        internalHostnames.map((h, i) => `${h}:${secondaryPorts[i]}`).join(",");
    const rsName = rst.name;
    const rsURL = `mongodb://${secondaryPortHosts}/${dbName}?replicaSet=${rsName}`;
    const evalScript = `
const coll = db.getSiblingDB(${tojson(dbName)}).getCollection(${tojson(collName)});
assert.commandWorked(coll.insert({_id: "secondaryPort", src: "secondaryPortConn"}));
const findResult = coll.findOne({_id: "secondaryPort"});
assert.neq(
    findResult, null, "should be able to read the document written via the secondary port");
assert.eq(findResult.src, "secondaryPortConn");
`;

    const res = runMongoProgram("mongo",
                                rsURL,
                                "--tls",
                                "--tlsAllowInvalidCertificates",
                                "--tlsCAFile",
                                caFile,
                                "--tlsCertificateKeyFile",
                                pemKeyFile,
                                "--eval",
                                evalScript);
    assert.eq(res, 0, "TLS secondary-port replica set shell program should succeed");
}

// should log a connection accepted on the secondary port
{
    const logs = checkLog.getGlobalLog(primary);
    const connectionAccepted = logs.some((line) => {
        return line.includes('"id":22943');
    });
    assert(connectionAccepted,
           "Expected at least one 'Connection accepted' (log 22943) entry in the server log");
}

rst.stopSet();
