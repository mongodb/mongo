/**
 * Tests authorization of applyOps command ops that resolve their target from the "ui" (UUID).
 *
 * For these ops, execution resolves the target from the "ui" while the command body ("o") may name a
 * different collection. Authorization is evaluated against the namespace that is actually acted upon:
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
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const kDbName = "apply_ops_uuid_ddl";
const kEveDb = "apply_ops_uuid_ddl_evil"; // a database eve is allowed to dropDatabase on
const kVictim = "victim"; // eve has NO rights on this collection
const kMine = "mine"; // eve may drop / modify / rename this collection
const kRenamed = kMine + "_renamed";

describe("applyOps authorization for UUID-targeted command ops", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({auth: ""});
        const admin = this.conn.getDB("admin");
        assert.commandWorked(admin.runCommand({createUser: "root", pwd: "pwd", roles: ["root"]}));
        assert(admin.auth("root", "pwd"));

        // eve may run applyOps with UUIDs, may drop/modify/rename ONLY her own collection in
        // kDbName, and may dropDatabase ONLY on kEveDb. She has nothing on the victim collection.
        assert.commandWorked(
            admin.runCommand({
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
            }),
        );
        assert.commandWorked(
            admin.runCommand({
                createUser: "eve",
                pwd: "pwd",
                roles: [{role: "eveRole", db: "admin"}],
            }),
        );
        admin.logout();
    });

    beforeEach(function () {
        const admin = this.conn.getDB("admin");
        assert(admin.auth("root", "pwd"));
        const db = this.conn.getDB(kDbName);

        db[kVictim].drop();
        db[kMine].drop();
        db[kRenamed].drop();
        assert.commandWorked(db[kVictim].insert({_id: 0, secret: "do-not-touch"}));
        assert.commandWorked(db[kMine].insert({_id: 0, mine: true}));
        assert.commandWorked(db[kVictim].createIndex({secret: 1}, {name: "secret_1"}));
        assert.commandWorked(db[kMine].createIndex({mine: 1}, {name: "mine_1"}));

        const eveDb = this.conn.getDB(kEveDb);
        eveDb.evecoll.drop();
        assert.commandWorked(eveDb.evecoll.insert({_id: 0}));
        this.eveCollUUID = eveDb.getCollectionInfos({name: "evecoll"})[0].info.uuid;

        this.mineUUID = db.getCollectionInfos({name: kMine})[0].info.uuid;
        this.victimUUID = db.getCollectionInfos({name: kVictim})[0].info.uuid;
        admin.logout();
    });

    afterEach(function () {
        this.conn.getDB(kDbName).logout();
        this.conn.getDB(kEveDb).logout();
        this.conn.getDB("admin").logout();
    });

    function asRoot(conn, fn) {
        assert(conn.getDB("admin").auth("root", "pwd"));
        try {
            return fn();
        } finally {
            conn.getDB("admin").logout();
        }
    }

    function collExists(conn, dbName, collName) {
        return asRoot(conn, () => conn.getDB(dbName).getCollectionInfos({name: collName}).length === 1);
    }

    function collHasIndex(conn, collName, indexName) {
        return asRoot(conn, () =>
            conn
                .getDB(kDbName)
                [collName].getIndexes()
                .some((i) => i.name === indexName),
        );
    }

    // Runs an applyOps command as eve (whose user doc lives on "admin"; auth is connection-wide).
    function asEve(conn, op) {
        assert(conn.getDB("admin").auth("eve", "pwd"));
        try {
            return conn.getDB(kDbName).runCommand({applyOps: [op]});
        } finally {
            conn.getDB("admin").logout();
        }
    }

    // ---- Rejected: ui points at a collection eve is not authorized for. ----

    it("rejects an applyOps drop whose ui points at an unauthorized collection", function () {
        const res = asEve(this.conn, {
            op: "c",
            ns: kDbName + "." + kMine,
            ui: this.victimUUID,
            o: {drop: kMine},
        });
        assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
        assert(collExists(this.conn, kDbName, kVictim), "victim must not be dropped");
    });

    it("rejects an applyOps dropIndexes whose ui points at an unauthorized collection", function () {
        const res = asEve(this.conn, {
            op: "c",
            ns: kDbName + "." + kMine,
            ui: this.victimUUID,
            o: {dropIndexes: kMine, index: "secret_1"},
        });
        assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
        assert(collHasIndex(this.conn, kVictim, "secret_1"), "victim's index must not be dropped");
    });

    it("rejects an applyOps collMod whose ui points at an unauthorized collection", function () {
        const res = asEve(this.conn, {
            op: "c",
            ns: kDbName + "." + kMine,
            ui: this.victimUUID,
            o: {collMod: kMine, index: {keyPattern: {secret: 1}, hidden: true}},
        });
        assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
    });

    it("rejects an applyOps renameCollection whose ui points at an unauthorized collection", function () {
        const res = asEve(this.conn, {
            op: "c",
            ns: kDbName + "." + kMine,
            ui: this.victimUUID,
            o: {
                renameCollection: kDbName + "." + kMine,
                to: kDbName + "." + kRenamed,
                dropTarget: false,
            },
        });
        assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
        assert(collExists(this.conn, kDbName, kVictim), "victim must not be renamed");
        assert(!collExists(this.conn, kDbName, kRenamed), "victim must not be renamed to an attacker name");
    });

    it("rejects an applyOps dropDatabase whose ui resolves to another database", function () {
        // ns names kDbName (which eve may not dropDatabase); ui resolves to a kEveDb collection
        // (which she may). A UUID override must not change the authorized database, so this is
        // rejected before execution.
        const res = asEve(this.conn, {
            op: "c",
            ns: kDbName + ".$cmd",
            ui: this.eveCollUUID,
            o: {dropDatabase: 1},
        });
        assert.commandFailedWithCode(res, ErrorCodes.Unauthorized);
        assert(collExists(this.conn, kDbName, kVictim), "victim database must not be dropped");
    });

    // ---- Still allowed: ui and o name the same authorized namespace. ----

    it("allows an applyOps drop whose ui and o agree on an authorized collection", function () {
        const res = asEve(this.conn, {
            op: "c",
            ns: kDbName + "." + kMine,
            ui: this.mineUUID,
            o: {drop: kMine},
        });
        assert.commandWorked(res);
        assert(!collExists(this.conn, kDbName, kMine), "eve's own collection should be dropped");
        assert(collExists(this.conn, kDbName, kVictim), "victim must remain untouched");
    });

    it("allows an applyOps dropIndexes whose ui and o agree on an authorized collection", function () {
        const res = asEve(this.conn, {
            op: "c",
            ns: kDbName + "." + kMine,
            ui: this.mineUUID,
            o: {dropIndexes: kMine, index: "mine_1"},
        });
        assert.commandWorked(res);
        assert(!collHasIndex(this.conn, kMine, "mine_1"), "eve's own index should be dropped");
    });

    it("allows an applyOps collMod whose ui and o agree on an authorized collection", function () {
        const res = asEve(this.conn, {
            op: "c",
            ns: kDbName + "." + kMine,
            ui: this.mineUUID,
            o: {collMod: kMine, index: {keyPattern: {mine: 1}, hidden: true}},
        });
        assert.commandWorked(res);
    });

    it("allows an applyOps renameCollection whose ui and o agree on an authorized collection", function () {
        const res = asEve(this.conn, {
            op: "c",
            ns: kDbName + "." + kMine,
            ui: this.mineUUID,
            o: {
                renameCollection: kDbName + "." + kMine,
                to: kDbName + "." + kRenamed,
                dropTarget: false,
            },
        });
        assert.commandWorked(res);
        assert(collExists(this.conn, kDbName, kRenamed), "eve's own collection should be renamed");
        assert(!collExists(this.conn, kDbName, kMine), "source should no longer exist after rename");
    });

    it("allows an applyOps dropDatabase on a database eve is authorized for", function () {
        assert(this.conn.getDB("admin").auth("eve", "pwd"));
        const res = this.conn.getDB(kEveDb).runCommand({
            applyOps: [{op: "c", ns: kEveDb + ".$cmd", o: {dropDatabase: 1}}],
        });
        this.conn.getDB("admin").logout();

        assert.commandWorked(res);
        assert(!collExists(this.conn, kEveDb, "evecoll"), "eve's own database should be dropped");
        assert(collExists(this.conn, kDbName, kVictim), "victim database must remain untouched");
    });

    // An authorized caller may drop a collection by UUID while naming a different (placeholder)
    // collection in "o" -- e.g.
    // jstests/noPassthrough/replication/apply_ops_overwrite_admin_system_version.js overwrites
    // admin.system.version this way.
    it("allows an authorized caller to drop by ui with a non-matching o name", function () {
        assert(this.conn.getDB("admin").auth("root", "pwd"));
        const res = this.conn.getDB(kDbName).runCommand({
            applyOps: [
                {
                    op: "c",
                    ns: kDbName + ".$cmd",
                    ui: this.victimUUID,
                    o: {drop: "placeholder_name"},
                },
            ],
        });
        this.conn.getDB("admin").logout();

        assert.commandWorked(res, "drop-by-UUID with a non-matching o name should be allowed");
        assert(!collExists(this.conn, kDbName, kVictim), "the UUID-resolved collection should be dropped");
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });
});
