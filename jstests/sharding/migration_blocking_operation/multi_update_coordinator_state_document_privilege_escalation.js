/**
 * No user lacking the implicit privilege on the pseudo namespace may reach the
 * MultiUpdateCoordinator state document namespace, by any access path, however much privilege it
 * holds on the namespace itself. Writing that namespace escalates privileges: on step-up,
 * PrimaryOnlyService rebuilds the coordinator under internal authorization and runs the stored
 * update through the cluster write path, so a forged document naming admin.system.users would
 * replacement-upsert a root user that the writer is not otherwise authorized to create.
 *
 * @tags: [
 *   # This test runs ShardingTest with a keyFile. The continuous stepdown thread's connection is
 *   # not authenticated, so replSetGetConfig/reconfig against the config server fail with
 *   # Unauthorized.
 *   does_not_support_stepdowns,
 * ]
 */
import {CollectionActions} from "jstests/auth/lib/collection_actions.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const keyFile = "jstests/libs/key1";

const dbName = "victimdb";
const collName = "orders";
const namespace = `${dbName}.${collName}`;
const stateDocumentsNs = "localMigrationBlockingOperations.multiUpdateCoordinators";
const attackerPassword = "alicepw";
const explicitAttackerPassword = "carolpw";
const routerAttackerPassword = "bobpw";
const backupPassword = "backuppw";
const restorePassword = "restorepw";

// The strongest form of the attack: a user holding every action the access paths below require
// directly on the state document namespace, and nothing else. Without the implicit privilege on the
// pseudo namespace, such a user would be able to take all of them.
const stateDocumentActions = [
    "find",
    "insert",
    "update",
    "remove",
    "collMod",
    "createIndex",
    "dropCollection",
    "convertToCapped",
];

// The other namespaces the rename, clone and view access paths need as their other side. Granting
// full access to them keeps every path failing because of the namespace under test rather than
// because of the scratch namespace it also touches.
function otherSidePrivileges() {
    return [
        {
            resource: {db: "config", collection: "collectionActionsScratch"},
            actions: stateDocumentActions,
        },
        {
            resource: {db: "config", collection: "collectionActionsView"},
            actions: stateDocumentActions.concat(["createCollection"]),
        },
    ];
}

// The SCRAM-SHA-256 credentials below correspond to the password "pwnpass".
const forgedStateId = UUID("66666666-6666-4666-8666-666666666666");
const forgedUser = {
    _id: "admin.pwn",
    userId: UUID("55555555-5555-4555-8555-555555555555"),
    user: "pwn",
    db: "admin",
    credentials: {
        "SCRAM-SHA-256": {
            iterationCount: 15000,
            salt: "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGw==",
            storedKey: "LY2mQQqUifrAs4Tdf+1Jza4eJ5KxjGQZwJL1LF5Vths=",
            serverKey: "hVTOjDSFnu6iDCczdMrgFEouO3f1cRFFtxvDx3ufjZY=",
        },
    },
    roles: [{role: "root", db: "admin"}],
    authenticationRestrictions: [],
};

const forgedStateDocument = {
    _id: forgedStateId,
    ns: "admin.system.users",
    updateCommand: {
        update: "system.users",
        updates: [{q: {_id: "admin.pwn"}, u: forgedUser, multi: false, upsert: true}],
        ordered: false,
    },
    isUpsert: true,
    mutableFields: {phase: "acquireSession"},
};

// Every command that can reach the state document namespace, in either direction. Writing state
// that the server will later replay with internal authority must be denied no matter which command
// is used to write it; and the reads matter too, since the stored session id and pending command
// are not the attacker's to see.
const deniedCommands = CollectionActions.all({
    db: "config",
    coll: stateDocumentsNs,
    document: forgedStateDocument,
    id: forgedStateId,
});

describe("MultiUpdateCoordinator state document privilege escalation", function () {
    let st;
    let attacker;
    let forgedStateWasPersisted;

    before(function () {
        st = new ShardingTest({name: jsTestName(), keyFile: keyFile, shards: {rs0: {nodes: 2}}});

        // The cluster administrator seeds data that the attacker must not be able to reach, and
        // creates the shard-local user that the attacker controls. clusterManager is not special
        // here; it is simply a role that holds 'insert' on the shard's 'config' database, which is
        // what made the escalation below reachable in the original report.
        authutil.asCluster(st.s, keyFile, () => {
            assert.commandWorked(st.s.adminCommand({shardCollection: namespace, key: {_id: 1}}));
            const seedDocuments = [];
            for (let i = 0; i < 10; ++i) {
                seedDocuments.push({_id: i, owner: "victim", protected: true});
            }
            assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert(seedDocuments));
        });

        authutil.asCluster(st.rs0.getPrimary(), keyFile, () => {
            st.rs0
                .getPrimary()
                .getDB("admin")
                .createUser({
                    user: "alice",
                    pwd: attackerPassword,
                    roles: [{role: "clusterManager", db: "admin"}],
                });
        });

        attacker = new Mongo(st.rs0.getPrimary().host);
        assert(attacker.getDB("admin").auth("alice", attackerPassword), "attacker authentication failed");
    });

    after(function () {
        st.stop();
    });

    // The attacker's role must grant no access to the victim data by any path, and no way to write
    // users. This is the precondition for everything below: if clusterManager could already reach
    // this data directly, the escalation assertions would prove nothing.
    describe("baseline privileges of a shard-local clusterManager", function () {
        function assertUnauthorized(database, command) {
            assert.commandFailedWithCode(attacker.getDB(database).runCommand(command), ErrorCodes.Unauthorized);
        }

        const victimCollectionPaths = CollectionActions.all({
            db: dbName,
            coll: collName,
            document: {_id: -1, injected: true},
            id: -1,
        });

        for (const {name, db, command} of victimCollectionPaths) {
            it(`cannot reach the victim collection via ${name}`, function () {
                assertUnauthorized(db, command);
            });
        }

        // Not swept with CollectionActions: 'admin.system.users' is a restricted namespace, so
        // several access paths into it are rejected outright rather than with Unauthorized. The
        // direct write is the one that matters, since it is what the forged state document replays.
        it("cannot write directly to admin.system.users", function () {
            assertUnauthorized("admin", {
                update: "system.users",
                updates: [{q: {_id: "admin.pwn"}, u: forgedUser, multi: false, upsert: true}],
            });
        });
    });

    // Sweep every access path with a shard-local user granted all of the actions they require
    // directly on the namespace, so that each path is denied by the namespace's own rule rather
    // than by an action the attacker happens to be missing.
    describe("state document namespace is denied to an explicitly privileged shard user", function () {
        let explicitAttacker;

        before(function () {
            authutil.asCluster(st.rs0.getPrimary(), keyFile, () => {
                const adminDb = st.rs0.getPrimary().getDB("admin");
                assert.commandWorked(
                    adminDb.runCommand({
                        createRole: "shardStateDocumentWriter",
                        privileges: [
                            {
                                resource: {db: "config", collection: stateDocumentsNs},
                                actions: stateDocumentActions,
                            },
                            ...otherSidePrivileges(),
                        ],
                        roles: [],
                    }),
                );
                adminDb.createUser({
                    user: "carol",
                    pwd: explicitAttackerPassword,
                    roles: [{role: "shardStateDocumentWriter", db: "admin"}],
                });
            });

            explicitAttacker = new Mongo(st.rs0.getPrimary().host);
            assert(
                explicitAttacker.getDB("admin").auth("carol", explicitAttackerPassword),
                "explicitly privileged attacker authentication failed",
            );
        });

        for (const {name, db, command} of deniedCommands) {
            it(`denies ${name}`, function () {
                assert.commandFailedWithCode(explicitAttacker.getDB(db).runCommand(command), ErrorCodes.Unauthorized);
            });
        }
    });

    // The same denials must hold on the router. A request that originates on mongos reaches the
    // shard as an internal client, so the router is the only place the user's own privileges are
    // checked; a shard-only fix would leave this path open. Use a cluster user holding an explicit
    // privilege on the state document namespace, which is the strongest form of the router-side
    // attack.
    describe("state document namespace is denied on the router", function () {
        let routerAttacker;

        before(function () {
            authutil.asCluster(st.s, keyFile, () => {
                const adminDb = st.s.getDB("admin");
                assert.commandWorked(
                    adminDb.runCommand({
                        createRole: "stateDocumentWriter",
                        privileges: [
                            {
                                resource: {db: "config", collection: stateDocumentsNs},
                                actions: stateDocumentActions,
                            },
                            ...otherSidePrivileges(),
                        ],
                        roles: [],
                    }),
                );
                adminDb.createUser({
                    user: "bob",
                    pwd: routerAttackerPassword,
                    roles: [{role: "stateDocumentWriter", db: "admin"}],
                });
            });

            routerAttacker = new Mongo(st.s.host);
            assert(
                routerAttacker.getDB("admin").auth("bob", routerAttackerPassword),
                "router attacker authentication failed",
            );
        });

        for (const {name, db, command, shardOnly} of deniedCommands) {
            if (shardOnly) {
                continue;
            }
            it(`denies ${name}`, function () {
                assert.commandFailedWithCode(routerAttacker.getDB(db).runCommand(command), ErrorCodes.Unauthorized);
            });
        }
    });

    // The backup and restore built-in roles hold the implicit privilege, so locking the namespace
    // down must not break them. These are shard-local users because the state documents live in the
    // shard's own 'config' database.
    describe("backup and restore roles keep their access", function () {
        let backupUser;
        let restoreUser;

        before(function () {
            authutil.asCluster(st.rs0.getPrimary(), keyFile, () => {
                const adminDb = st.rs0.getPrimary().getDB("admin");
                adminDb.createUser({
                    user: "backupUser",
                    pwd: backupPassword,
                    roles: [{role: "backup", db: "admin"}],
                });
                adminDb.createUser({
                    user: "restoreUser",
                    pwd: restorePassword,
                    roles: [{role: "restore", db: "admin"}],
                });
            });

            backupUser = new Mongo(st.rs0.getPrimary().host);
            assert(backupUser.getDB("admin").auth("backupUser", backupPassword), "backup user auth failed");

            restoreUser = new Mongo(st.rs0.getPrimary().host);
            assert(restoreUser.getDB("admin").auth("restoreUser", restorePassword), "restore user auth failed");
        });

        // 'backup' grants find on the 'config' database, and now on the pseudo namespace as well.
        it("backup can read the state document namespace", function () {
            const configDb = backupUser.getDB("config");
            assert.commandWorked(configDb.runCommand({find: stateDocumentsNs, filter: {}}));
            assert.commandWorked(configDb.runCommand({aggregate: stateDocumentsNs, pipeline: [], cursor: {}}));
            assert.commandWorked(configDb.runCommand({count: stateDocumentsNs, query: {}}));
        });

        // 'restore' grants insert on the 'config' database — but not update or remove — so only the
        // insert is asserted here. A restore legitimately repopulates this namespace from a backup.
        it("restore can insert into the state document namespace", function () {
            const benignStateId = UUID("11111111-1111-4111-8111-111111111111");
            assert.commandWorked(
                restoreUser.getDB("config").runCommand({
                    insert: stateDocumentsNs,
                    documents: [
                        {
                            _id: benignStateId,
                            ns: namespace,
                            updateCommand: {
                                update: collName,
                                updates: [{q: {_id: -1}, u: {$set: {restored: true}}, multi: true}],
                                ordered: false,
                            },
                            isUpsert: false,
                            mutableFields: {phase: "unused"},
                        },
                    ],
                    writeConcern: {w: "majority"},
                }),
            );

            // Remove the restored document before the step-up below, so that it is not replayed and
            // cannot interfere with the escalation assertions that follow.
            authutil.asCluster(st.rs0.getPrimary(), keyFile, () => {
                assert.commandWorked(
                    st.rs0
                        .getPrimary()
                        .getDB("config")
                        .runCommand({
                            delete: stateDocumentsNs,
                            deletes: [{q: {_id: benignStateId}, limit: 1}],
                            writeConcern: {w: "majority"},
                        }),
                );
            });
        });
    });

    describe("step-up after a forged state document insert", function () {
        before(function () {
            // The plain insert from the original report, direct to the shard primary.
            const insertForgedState = attacker.getDB("config").runCommand({
                insert: stateDocumentsNs,
                documents: [forgedStateDocument],
                writeConcern: {w: "majority"},
            });
            jsTest.log.info("Forged state document insert result", {result: insertForgedState});
            forgedStateWasPersisted = insertForgedState.ok === 1 && !insertForgedState.writeErrors;

            // Trigger the primary transition that makes PrimaryOnlyService rebuild the forged
            // coordinator.
            authutil.asCluster(st.rs0.nodes, keyFile, () => {
                const newPrimary = st.rs0.getSecondaries()[0];
                st.rs0.stepUp(newPrimary);

                if (forgedStateWasPersisted) {
                    // Wait for the rebuilt coordinator to run its stored update to completion and
                    // clean up.
                    assert.soon(
                        () =>
                            st.rs0
                                .getPrimary()
                                .getDB("config")
                                .getCollection(stateDocumentsNs)
                                .countDocuments({_id: forgedStateId}) === 0,
                        "forged coordinator state document was never processed or removed",
                        5 * 60 * 1000,
                    );
                }
            });
        });

        it("does not create the forged root user", function () {
            authutil.asCluster(st.s, keyFile, () => {
                const users = st.s.getDB("admin").system.users.find({user: "pwn"}).toArray();
                assert.eq(
                    0,
                    users.length,
                    "clusterManager on a shard escalated to a cluster root user via forged" + " coordinator state",
                    {forgedStateWasPersisted, users},
                );
            });
        });

        it("leaves the victim data untouched", function () {
            authutil.asCluster(st.s, keyFile, () => {
                const victimCollection = st.s.getDB(dbName).getCollection(collName);
                assert.eq(10, victimCollection.countDocuments({owner: "victim"}));
            });
        });

        it("does not let the forged user authenticate through mongos", function () {
            const escalated = new Mongo(st.s.host).getDB("admin");
            assert(
                !escalated.auth({user: "pwn", pwd: "pwnpass", mechanism: "SCRAM-SHA-256"}),
                "forged root user authenticated through mongos",
            );
        });
    });
});
