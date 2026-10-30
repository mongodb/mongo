/**
 * Tests that 'showSystemEvents' change streams do not require privileges beyond those of the
 * stream's scope, and that events on system.js are only returned to users who are authorized to
 * read system.js. Users without that authorization can open the stream but do not see system.js
 * events, while events on other collections remain visible.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_replication,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */

const kKeyFile = "jstests/libs/key1";
const kPassword = "password";
const kDbName = jsTestName();
const kCollName = "coll";
const kAnyNormalReadPrivilege = {
    resource: {db: "", collection: ""},
    actions: ["find", "changeStream"],
};
const kSystemJsReadPrivilege = {
    resource: {db: "", collection: "system.js"},
    actions: ["find", "changeStream"],
};

// Creates the admin user, a user that may read every normal collection but not system.js, and a
// user that may additionally read system.js in every database.
function createUsers(conn) {
    const adminDB = conn.getDB("admin");
    assert.commandWorked(
        adminDB.runCommand({createUser: "admin", pwd: kPassword, roles: ["root"]}));
    assert(adminDB.auth("admin", kPassword));
    assert.commandWorked(
        adminDB.runCommand(
            {createRole: "readNormal", privileges: [kAnyNormalReadPrivilege], roles: []}),
    );
    assert.commandWorked(
        adminDB.runCommand({
            createRole: "readNormalAndSystemJs",
            privileges: [kAnyNormalReadPrivilege, kSystemJsReadPrivilege],
            roles: [],
        }),
    );
    assert.commandWorked(
        adminDB.runCommand({createUser: "noSystemJs", pwd: kPassword, roles: ["readNormal"]}));
    assert.commandWorked(
        adminDB.runCommand(
            {createUser: "withSystemJs", pwd: kPassword, roles: ["readNormalAndSystemJs"]}),
    );
    assert.commandWorked(adminDB.getSiblingDB(kDbName).createCollection(kCollName));
    adminDB.logout();
}

// Returns a connection to 'host' authenticated as 'user'.
function connectAs(host, user) {
    const conn = new Mongo(host);
    assert(conn.getDB("admin").auth(user, kPassword));
    return conn;
}

// Opens a 'showSystemEvents' change stream as 'user', either on the test database or on the whole
// cluster. Opening the stream must succeed regardless of the user's system.js privileges. If
// 'passthroughToShard' is set, the stream is opened on mongos as a per-shard cursor.
function openStream(host, user, wholeCluster, passthroughToShard) {
    const conn = connectAs(host, user);
    const db = conn.getDB(wholeCluster ? "admin" : kDbName);
    const res = assert.commandWorked(
        db.runCommand({
            aggregate: 1,
            pipeline:
                [{$changeStream: {allChangesForCluster: wholeCluster, showSystemEvents: true}}],
            cursor: {},
            ...(passthroughToShard ? {$_passthroughToShard: {shard: passthroughToShard}} : {}),
        }),
    );
    return {conn, cursor: new DBCommandCursor(db, res)};
}

// Returns the events of 'cursor' up to and including the insert of 'sentinelId' into the test
// collection.
function readUntilSentinel(cursor, sentinelId) {
    const events = [];
    assert.soon(() => {
        // 'hasNext()' returns false once a getMore comes back empty, so poll until the sentinel.
        while (cursor.hasNext()) {
            const event = cursor.next();
            events.push(event);
            if (event.operationType === "insert" && event.ns.coll === kCollName &&
                event.documentKey._id === sentinelId) {
                return true;
            }
        }
        return false;
    });
    return events;
}

function eventsOn(events, collName) {
    return events.filter((event) => event.ns && event.ns.coll === collName);
}

// Shared by all scenarios, since several of them may write to the same cluster.
let sentinelId = 0;

function runScenarios(getHost, getPassthroughShard) {
    // Performs writes on system.js followed by a sentinel insert into a regular collection. Returns
    // the sentinel's _id.
    function writeSystemEvents() {
        const conn = connectAs(getHost(), "admin");
        const db = conn.getDB(kDbName);
        const id = ++sentinelId;
        assert.commandWorked(db.system.js.insert({_id: id, value: "secret"}));
        assert.commandWorked(db.system.js.update({_id: id}, {$set: {value: "updated secret"}}));
        assert.commandWorked(db.system.js.remove({_id: id}));
        assert.commandWorked(db.getCollection(kCollName).insert({_id: id}));
        conn.close();
        return id;
    }

    for (const wholeCluster of [false, true]) {
        const scope = (wholeCluster ? "whole-cluster" : "whole-db") +
            (getPassthroughShard ? " per-shard" : "");
        const passthroughToShard = getPassthroughShard ? getPassthroughShard() : undefined;

        // A user without system.js privileges can open the stream but sees no system.js events.
        let {conn, cursor} = openStream(getHost(), "noSystemJs", wholeCluster, passthroughToShard);
        let events = readUntilSentinel(cursor, writeSystemEvents());
        assert.eq(eventsOn(events, "system.js"), [], {events});
        cursor.close();
        conn.close();

        // A user with system.js privileges sees the system.js insert, update and delete.
        let withSystemJs = openStream(getHost(), "withSystemJs", wholeCluster, passthroughToShard);
        events = readUntilSentinel(withSystemJs.cursor, writeSystemEvents());
        assert.eq(
            eventsOn(events, "system.js").map((event) => event.operationType),
            ["insert", "update", "delete"],
            {events},
        );
        withSystemJs.cursor.close();
        withSystemJs.conn.close();

        jsTest.log("Passed " + scope + " showSystemEvents scenarios");
    }
}

// Replica set scenarios.
let rst = new ReplSetTest({nodes: 1});
rst.startSet({keyFile: kKeyFile});
rst.initiate();
createUsers(rst.getPrimary());
runScenarios(() => rst.getPrimary().host);
rst.stopSet();

// Sharded cluster scenarios.
let st = new ShardingTest({
    shards: 2,
    mongos: 1,
    // Idle shards must advance their oplog for the router to return events from other shards.
    rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
    other: {keyFile: kKeyFile},
});
createUsers(st.s);
runScenarios(() => st.s.host);
// With '$_passthroughToShard' the router forwards the stream to the shard unparsed, so the
// system.js filtering must not depend on the shard knowing the user. Target the primary shard
// of the test database, which receives its system.js writes.
runScenarios(() => st.s.host, () => {
    const conn = connectAs(st.s.host, "admin");
    const primary = conn.getDB("config").databases.findOne({_id: kDbName}).primary;
    conn.close();
    return primary;
});
st.stop();
