/**
 * Tests authorization of applyOps command ops that resolve their target from the "ui" (UUID).
 *
 * For these ops, execution resolves the target from the "ui" while the command body ("o") may name
 * a different collection. Authorization is evaluated against the namespace that is actually acted
 * upon:
 *   - drop / collMod / dropIndexes / renameCollection: the caller must be authorized for the
 *     command's action on the collection the "ui" resolves to.
 *   - dropDatabase: execution ignores the "ui" and drops the "ns" database, so authorization is
 *     evaluated against that database.
 * A caller with rights only on their own namespace cannot target a namespace they lack rights on by
 * pointing "ui" at it (the op fails with Unauthorized), while a caller authorized for the resolved
 * target may target it by UUID even when "o" names a different collection.
 *
 * @tags: [requires_auth]
 */
(function() {
"use strict";

const kDbName = "apply_ops_uuid_ddl";
const kEveDb = "apply_ops_uuid_ddl_evil";  // a database eve is allowed to dropDatabase on
const kVictim = "victim";                  // eve has NO rights on this collection
const kMine = "mine";                      // eve may drop / modify / rename this collection
const kRenamed = kMine + "_renamed";

const conn = MongoRunner.runMongod({auth: ""});
const admin = conn.getDB("admin");
assert.commandWorked(admin.runCommand({createUser: "root", pwd: "pwd", roles: ["root"]}));
assert(admin.auth("root", "pwd"));

// eve may run applyOps with UUIDs, may drop/modify/rename ONLY her own collection in kDbName, and
// may dropDatabase ONLY on kEveDb. She has nothing on the victim collection.
assert.commandWorked(admin.runCommand({
    createRole: "eveRole",
    privileges: [
        {resource: {cluster: true}, actions: ["applyOps", "useUUID"]},
        {
            resource: {db: kDbName, collection: ""},
            actions: ["listCollections", "renameCollectionSameDB"],
        },
        {
            resource: {db: kDbName, collection: kMine},
            actions: [
                "createCollection",
                "createIndex",
                "dropCollection",
                "dropIndex",
                "collMod",
                "find",
                "insert",
            ],
        },
        // rename target also lives in kDbName; grant read/write on it.
        {
            resource: {db: kDbName, collection: kRenamed},
            actions: ["createCollection", "createIndex", "find", "insert"],
        },
        {
            resource: {db: kEveDb, collection: ""},
            actions: ["dropDatabase", "listCollections"],
        },
    ],
    roles: [],
}));
assert.commandWorked(
    admin.runCommand({createUser: "eve", pwd: "pwd", roles: [{role: "eveRole", db: "admin"}]}));
admin.logout();

function asRoot(fn) {
    assert(conn.getDB("admin").auth("root", "pwd"));
    try {
        return fn();
    } finally {
        conn.getDB("admin").logout();
    }
}

function collExists(dbName, collName) {
    return asRoot(() => conn.getDB(dbName).getCollectionInfos({name: collName}).length === 1);
}

function collHasIndex(collName, indexName) {
    return asRoot(() =>
                      conn.getDB(kDbName)[collName].getIndexes().some((i) => i.name === indexName));
}

// Runs an applyOps command as eve (whose user doc lives on "admin"; auth is connection-wide).
function asEve(op) {
    assert(conn.getDB("admin").auth("eve", "pwd"));
    try {
        return conn.getDB(kDbName).runCommand({applyOps: [op]});
    } finally {
        conn.getDB("admin").logout();
    }
}

// Recreates the fixture collections and returns the UUIDs the tests target. Called before each
// case so state does not leak between them.
function resetFixture() {
    return asRoot(() => {
        const db = conn.getDB(kDbName);
        db[kVictim].drop();
        db[kMine].drop();
        db[kRenamed].drop();
        assert.commandWorked(db[kVictim].insert({_id: 0, secret: "do-not-touch"}));
        assert.commandWorked(db[kMine].insert({_id: 0, mine: true}));
        assert.commandWorked(db[kVictim].createIndex({secret: 1}, {name: "secret_1"}));
        assert.commandWorked(db[kMine].createIndex({mine: 1}, {name: "mine_1"}));

        const eveDb = conn.getDB(kEveDb);
        eveDb.evecoll.drop();
        assert.commandWorked(eveDb.evecoll.insert({_id: 0}));

        return {
            mineUUID: db.getCollectionInfos({name: kMine})[0].info.uuid,
            victimUUID: db.getCollectionInfos({name: kVictim})[0].info.uuid,
            eveCollUUID: eveDb.getCollectionInfos({name: "evecoll"})[0].info.uuid,
        };
    });
}

function runTest(name, fn) {
    jsTest.log("Running case: " + name);
    fn(resetFixture());
}

// ---- Rejected: ui points at a collection eve is not authorized for. ----

runTest("rejects an applyOps drop whose ui points at an unauthorized collection", function(uuids) {
    const res = asEve({op: "c", ns: kDbName + "." + kMine, ui: uuids.victimUUID, o: {drop: kMine}});
    assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
    assert(collExists(kDbName, kVictim), "victim must not be dropped");
});

runTest("rejects an applyOps dropIndexes whose ui points at an unauthorized collection",
        function(uuids) {
            const res = asEve({
                op: "c",
                ns: kDbName + "." + kMine,
                ui: uuids.victimUUID,
                o: {dropIndexes: kMine, index: "secret_1"},
            });
            assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
            assert(collHasIndex(kVictim, "secret_1"), "victim's index must not be dropped");
        });

runTest("rejects an applyOps collMod whose ui points at an unauthorized collection",
        function(uuids) {
            const res = asEve({
                op: "c",
                ns: kDbName + "." + kMine,
                ui: uuids.victimUUID,
                o: {collMod: kMine, index: {keyPattern: {secret: 1}, hidden: true}},
            });
            assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
        });

runTest("rejects an applyOps renameCollection whose ui points at an unauthorized collection",
        function(uuids) {
            const res = asEve({
                op: "c",
                ns: kDbName + "." + kMine,
                ui: uuids.victimUUID,
                o: {
                    renameCollection: kDbName + "." + kMine,
                    to: kDbName + "." + kRenamed,
                    dropTarget: false,
                },
            });
            assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
            assert(collExists(kDbName, kVictim), "victim must not be renamed");
            assert(!collExists(kDbName, kRenamed),
                   "victim must not be renamed to an attacker name");
        });

runTest("rejects an applyOps dropDatabase whose ui resolves to another database", function(uuids) {
    // ns names kDbName (which eve may not dropDatabase); ui resolves to a kEveDb collection (which
    // she may). A UUID override must not change the authorized database, so this is rejected before
    // execution.
    const res =
        asEve({op: "c", ns: kDbName + ".$cmd", ui: uuids.eveCollUUID, o: {dropDatabase: 1}});
    assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
    assert(collExists(kDbName, kVictim), "victim database must not be dropped");
});

// ---- Still allowed: ui and o name the same authorized namespace. ----

runTest("allows an applyOps drop whose ui and o agree on an authorized collection",
        function(uuids) {
            const res =
                asEve({op: "c", ns: kDbName + "." + kMine, ui: uuids.mineUUID, o: {drop: kMine}});
            assert.commandWorked(res);
            assert(!collExists(kDbName, kMine), "eve's own collection should be dropped");
            assert(collExists(kDbName, kVictim), "victim must remain untouched");
        });

runTest("allows an applyOps dropIndexes whose ui and o agree on an authorized collection",
        function(uuids) {
            const res = asEve({
                op: "c",
                ns: kDbName + "." + kMine,
                ui: uuids.mineUUID,
                o: {dropIndexes: kMine, index: "mine_1"},
            });
            assert.commandWorked(res);
            assert(!collHasIndex(kMine, "mine_1"), "eve's own index should be dropped");
        });

runTest("allows an applyOps collMod whose ui and o agree on an authorized collection",
        function(uuids) {
            const res = asEve({
                op: "c",
                ns: kDbName + "." + kMine,
                ui: uuids.mineUUID,
                o: {collMod: kMine, index: {keyPattern: {mine: 1}, hidden: true}},
            });
            assert.commandWorked(res);
        });

runTest("allows an applyOps renameCollection whose ui and o agree on an authorized collection",
        function(uuids) {
            const res = asEve({
                op: "c",
                ns: kDbName + "." + kMine,
                ui: uuids.mineUUID,
                o: {
                    renameCollection: kDbName + "." + kMine,
                    to: kDbName + "." + kRenamed,
                    dropTarget: false,
                },
            });
            assert.commandWorked(res);
            assert(collExists(kDbName, kRenamed), "eve's own collection should be renamed");
            assert(!collExists(kDbName, kMine), "source should no longer exist after rename");
        });

runTest("allows an applyOps dropDatabase on a database eve is authorized for", function(uuids) {
    assert(conn.getDB("admin").auth("eve", "pwd"));
    const res = conn.getDB(kEveDb).runCommand(
        {applyOps: [{op: "c", ns: kEveDb + ".$cmd", o: {dropDatabase: 1}}]});
    conn.getDB("admin").logout();

    assert.commandWorked(res);
    assert(!collExists(kEveDb, "evecoll"), "eve's own database should be dropped");
    assert(collExists(kDbName, kVictim), "victim database must remain untouched");
});

// An authorized caller may drop a collection by UUID while naming a different (placeholder)
// collection in "o" -- e.g.
// jstests/noPassthrough/apply_ops_overwrite_admin_system_version.js overwrites admin.system.version
// this way.
runTest("allows an authorized caller to drop by ui with a non-matching o name", function(uuids) {
    assert(conn.getDB("admin").auth("root", "pwd"));
    const res = conn.getDB(kDbName).runCommand({
        applyOps:
            [{op: "c", ns: kDbName + ".$cmd", ui: uuids.victimUUID, o: {drop: "placeholder_name"}}],
    });
    conn.getDB("admin").logout();

    assert.commandWorked(res, "drop-by-UUID with a non-matching o name should be allowed");
    assert(!collExists(kDbName, kVictim), "the UUID-resolved collection should be dropped");
});

MongoRunner.stopMongod(conn);
})();
