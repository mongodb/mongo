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
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {listIdleCursors} from "jstests/libs/query/change_stream_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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
        adminDB.runCommand({createUser: "admin", pwd: kPassword, roles: ["root"]}),
    );
    assert(adminDB.auth("admin", kPassword));
    assert.commandWorked(
        adminDB.runCommand({
            createRole: "readNormal",
            privileges: [kAnyNormalReadPrivilege],
            roles: [],
        }),
    );
    assert.commandWorked(
        adminDB.runCommand({
            createRole: "readNormalAndSystemJs",
            privileges: [kAnyNormalReadPrivilege, kSystemJsReadPrivilege],
            roles: [],
        }),
    );
    assert.commandWorked(
        adminDB.runCommand({createUser: "noSystemJs", pwd: kPassword, roles: ["readNormal"]}),
    );
    assert.commandWorked(
        adminDB.runCommand({
            createUser: "withSystemJs",
            pwd: kPassword,
            roles: ["readNormalAndSystemJs"],
        }),
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
// 'targetShard' is set, the stream is opened on mongos as a per-shard cursor on that shard.
function openStream(host, user, {wholeCluster, targetShard, comment}) {
    const conn = connectAs(host, user);
    const db = conn.getDB(wholeCluster ? "admin" : kDbName);
    const res = assert.commandWorked(
        db.runCommand({
            aggregate: 1,
            pipeline: [
                {$changeStream: {allChangesForCluster: wholeCluster, showSystemEvents: true}},
            ],
            cursor: {},
            comment,
            ...(targetShard ? {$_passthroughToShard: {shard: targetShard}} : {}),
        }),
    );
    return {conn, cursor: new DBCommandCursor(db, res)};
}

// The stage the router adds after $changeStream to drop system.js events from a per-shard stream
// of a user who may not read system.js.
const kSystemJsFilter = {
    $match: {"ns.coll": {$ne: "system.js"}, "to.coll": {$ne: "system.js"}},
};

// Returns the pipeline that 'shard' runs for the change stream opened with 'comment'.
function getShardPipeline(host, shard, comment) {
    const conn = connectAs(host, "admin");
    let cursors;
    assert.soon(() => {
        cursors = listIdleCursors(conn.getDB("admin"), {
            shard,
            "cursor.originatingCommand.comment": comment,
        });
        return cursors.length > 0;
    });
    conn.close();
    assert.eq(cursors.length, 1, {cursors});
    return cursors[0].cursor.originatingCommand.pipeline;
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
            if (
                event.operationType === "insert" &&
                event.ns.coll === kCollName &&
                event.documentKey._id === sentinelId
            ) {
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

// Runs the scenarios against 'cluster.host'. If 'cluster.targetShard' is set, the streams are
// per-shard streams on that shard. Both fields are read when the tests run, so they can be set in a
// 'before' block.
function runTests(cluster) {
    // Performs writes on system.js followed by a sentinel insert into a regular collection. Returns
    // the sentinel's _id.
    function writeSystemEvents() {
        const conn = connectAs(cluster.host, "admin");
        const db = conn.getDB(kDbName);
        const id = ++sentinelId;
        assert.commandWorked(db.system.js.insert({_id: id, value: "secret"}));
        assert.commandWorked(db.system.js.update({_id: id}, {$set: {value: "updated secret"}}));
        assert.commandWorked(db.system.js.remove({_id: id}));
        assert.commandWorked(db.getCollection(kCollName).insert({_id: id}));
        conn.close();
        return id;
    }

    // Asserts that the router added the system.js filter to the per-shard stream opened with
    // 'comment' if and only if 'expectFilter' is set. No-op for streams that are not per-shard.
    function assertShardFilter(comment, expectFilter) {
        if (!cluster.targetShard) {
            return;
        }
        const pipeline = getShardPipeline(cluster.host, cluster.targetShard, comment);
        assert(pipeline[0].$changeStream, {pipeline});
        if (expectFilter) {
            assert.docEq(kSystemJsFilter, pipeline[1], {pipeline});
        } else {
            assert(!pipeline.some((stage) => bsonWoCompare(stage, kSystemJsFilter) === 0), {
                pipeline,
            });
        }
    }

    for (const wholeCluster of [false, true]) {
        const scope = wholeCluster ? "whole-cluster" : "whole-db";

        it(`hides system.js events from a ${scope} stream without system.js privileges`, function () {
            const comment = `hides ${scope} ${++sentinelId}`;
            const {conn, cursor} = openStream(cluster.host, "noSystemJs", {
                wholeCluster,
                targetShard: cluster.targetShard,
                comment,
            });
            assertShardFilter(comment, true);
            const events = readUntilSentinel(cursor, writeSystemEvents());

            assert.eq(eventsOn(events, "system.js"), [], {events});
            cursor.close();
            conn.close();
        });

        it(`shows system.js events to a ${scope} stream with system.js privileges`, function () {
            const comment = `shows ${scope} ${++sentinelId}`;
            const {conn, cursor} = openStream(cluster.host, "withSystemJs", {
                wholeCluster,
                targetShard: cluster.targetShard,
                comment,
            });
            assertShardFilter(comment, false);
            const events = readUntilSentinel(cursor, writeSystemEvents());

            const systemJsEvents = eventsOn(events, "system.js");
            assert.eq(
                systemJsEvents.map((event) => event.operationType),
                ["insert", "update", "delete"],
                {events},
            );
            cursor.close();
            conn.close();
        });
    }
}

describe("showSystemEvents change stream authorization on a replica set", function () {
    let rst;
    const cluster = {};

    before(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet({keyFile: kKeyFile});
        rst.initiate();
        createUsers(rst.getPrimary());
        cluster.host = rst.getPrimary().host;
    });

    after(function () {
        rst.stopSet();
    });

    runTests(cluster);
});

describe("showSystemEvents change stream authorization on a sharded cluster", function () {
    let st;
    const cluster = {};

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongos: 1,
            // Idle shards must advance their oplog for the router to return events from other shards.
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
            other: {keyFile: kKeyFile},
        });
        createUsers(st.s);
        cluster.host = st.s.host;
    });

    after(function () {
        st.stop();
    });

    runTests(cluster);

    // With '$_passthroughToShard' the router forwards the stream to the shard unparsed, so the
    // system.js filtering must not depend on the shard knowing the user.
    describe("with $_passthroughToShard", function () {
        const perShardCluster = {};

        before(function () {
            // Target the primary shard of the test database, which receives its system.js writes.
            const conn = connectAs(st.s.host, "admin");
            perShardCluster.host = st.s.host;
            perShardCluster.targetShard = conn
                .getDB("config")
                .databases.findOne({_id: kDbName}).primary;
            conn.close();
        });

        runTests(perShardCluster);
    });
});
