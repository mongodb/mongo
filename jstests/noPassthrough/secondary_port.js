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

const secondaryPort = allocatePort();

const rst = new ReplSetTest({
    nodes: [{secondaryPort: secondaryPort}],
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const mainPort = primary.port;
const host = primary.hostNoPort;

// should respond to commands on the main port
{
    const mainConn = new Mongo(`${host}:${mainPort}`);
    const res = assert.commandWorked(mainConn.adminCommand({ping: 1}));
    assert.eq(res.ok, 1, "ping on main port should succeed");

    const helloRes = assert.commandWorked(mainConn.adminCommand({hello: 1}));
    assert(helloRes.isWritablePrimary, "node should be writable primary on main port");

    mainConn.close();
}

// should respond to commands on the secondary port
{
    const secondaryConn = new Mongo(`${host}:${secondaryPort}`);
    const res = assert.commandWorked(secondaryConn.adminCommand({ping: 1}));
    assert.eq(res.ok, 1, "ping on secondary port should succeed");

    const helloRes = assert.commandWorked(secondaryConn.adminCommand({hello: 1}));
    assert(helloRes.isWritablePrimary, "node should be writable primary on secondary port");

    secondaryConn.close();
}

// should make writes on the main port visible from the secondary port
{
    const dbName = jsTestName();
    const collName = "testcoll";

    const mainConn = new Mongo(`${host}:${mainPort}`);
    const mainDB = mainConn.getDB(dbName);
    assert.commandWorked(mainDB.getCollection(collName).insert({_id: 1, x: "fromMainPort"}));

    const secondaryConn = new Mongo(`${host}:${secondaryPort}`);
    const secondaryDB = secondaryConn.getDB(dbName);
    const doc = secondaryDB.getCollection(collName).findOne({_id: 1});
    assert.neq(doc, null, "document inserted via main port should be readable via secondary port");
    assert.eq(doc.x, "fromMainPort");

    mainConn.close();
    secondaryConn.close();
}

rst.stopSet();
