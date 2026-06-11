/**
 * Tests that killCursors authorizes against the cursor's *stored* namespace rather than the
 * client-supplied request namespace. This serves as a regression test for SERVER-128198.
 *
 * The killCursors is authorized if any of the following are true:
 * - Caller has cluster-level killAnyCursor (namespace-independent).
 * - Caller is coauthorized with the cursor owner (self-kill; namespace-independent).
 * - Caller has killAnyCursor on the cursor's *stored* namespace (db or exact collection).
 *
 * The authorization contract (auth::checkAuthForKillCursors) admits a kill when any of:
 *   1. Caller has cluster-level killAnyCursor (namespace-independent).
 *   2. Caller is coauthorized with the cursor owner (self-kill, caller owns the cursor being killed).
 *   3. Caller has killAnyCursor on the cursor's *stored* namespace (db or exact collection).
 *
 * The test runs against both a standalone mongod and a sharded cluster, providing coverage for
 * both topologies in one file.
 *
 * @tags: [requires_sharding]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Multiple users can't be authenticated on one connection within a session, so we need separate connections for each user.
TestData.disableImplicitSessions = true;

/**
 * Registers a describe-block that tests killCursors namespace authorization for one topology.
 */
function registerTests(topologyName, startFn) {
    describe(`killCursors namespace authorization (${topologyName})`, function () {
        // Set in before(); referenced by tryKill via closure.
        let topology;
        let adminConn;
        let noPrivConn, aliceConn, bobConn;
        let dbAKillerConn, collA1KillerConn, clusterKillerConn;

        before(function () {
            topology = startFn();
            const conn = topology.conn;

            // Bootstrap the first admin user (no-auth required when no users exist yet).
            const bootstrapAdmin = conn.getDB("admin");
            assert.commandWorked(
                bootstrapAdmin.runCommand({createUser: "admin", pwd: "pwd", roles: [{role: "root", db: "admin"}]}),
            );
            assert(bootstrapAdmin.auth("admin", "pwd"));

            // Insert enough documents so that batchSize:2 leaves a live server-side cursor.
            for (let i = 0; i < 101; i++) {
                assert.commandWorked(conn.getDB("dbA").collA1.insert({_id: i}));
                assert.commandWorked(conn.getDB("dbA").collA2.insert({_id: i}));
                assert.commandWorked(conn.getDB("dbB").collB1.insert({_id: i}));
            }

            // Custom roles granting killAnyCursor at different scopes.
            assert.commandWorked(
                bootstrapAdmin.runCommand({
                    createRole: "dbAKillRole",
                    privileges: [{resource: {db: "dbA", collection: ""}, actions: ["killAnyCursor"]}],
                    roles: [],
                }),
            );
            assert.commandWorked(
                bootstrapAdmin.runCommand({
                    createRole: "collA1KillRole",
                    privileges: [{resource: {db: "dbA", collection: "collA1"}, actions: ["killAnyCursor"]}],
                    roles: [],
                }),
            );
            assert.commandWorked(
                bootstrapAdmin.runCommand({
                    createRole: "clusterKillRole",
                    privileges: [{resource: {cluster: true}, actions: ["killAnyCursor"]}],
                    roles: [],
                }),
            );

            // Users.
            assert.commandWorked(
                bootstrapAdmin.runCommand({createUser: "noPriv", pwd: "pwd", roles: [{role: "read", db: "dbA"}]}),
            );
            assert.commandWorked(
                bootstrapAdmin.runCommand({createUser: "alice", pwd: "pwd", roles: [{role: "read", db: "dbA"}]}),
            );
            assert.commandWorked(
                bootstrapAdmin.runCommand({createUser: "bob", pwd: "pwd", roles: [{role: "read", db: "dbB"}]}),
            );
            assert.commandWorked(
                bootstrapAdmin.runCommand({
                    createUser: "dbAKiller",
                    pwd: "pwd",
                    roles: [
                        {role: "read", db: "dbA"},
                        {role: "dbAKillRole", db: "admin"},
                    ],
                }),
            );
            assert.commandWorked(
                bootstrapAdmin.runCommand({
                    createUser: "collA1Killer",
                    pwd: "pwd",
                    roles: [
                        {role: "read", db: "dbA"},
                        {role: "collA1KillRole", db: "admin"},
                    ],
                }),
            );
            assert.commandWorked(
                bootstrapAdmin.runCommand({
                    createUser: "clusterKiller",
                    pwd: "pwd",
                    roles: [
                        {role: "read", db: "dbB"},
                        {role: "clusterKillRole", db: "admin"},
                    ],
                }),
            );

            bootstrapAdmin.logout();

            // Open one persistent authenticated connection per user.
            const openAuth = (user) => {
                const c = new Mongo(conn.host);
                assert(c.getDB("admin").auth(user, "pwd"));
                return c;
            };
            noPrivConn = openAuth("noPriv");
            aliceConn = openAuth("alice");
            bobConn = openAuth("bob");
            dbAKillerConn = openAuth("dbAKiller");
            collA1KillerConn = openAuth("collA1Killer");
            clusterKillerConn = openAuth("clusterKiller");
            adminConn = openAuth("admin");
        });

        after(function () {
            topology.stop();
        });

        /**
         * Opens a cursor as openerConn on cursorDb.cursorColl (batchSize:2), then has killerConn
         * issue killCursors against requestDb.requestColl with that cursor id.
         *
         * If expectKilled:
         *   - Asserts the kill command succeeds.
         *   - Confirms the cursor is dead via a follow-up getMore (expects CursorNotFound).
         * Otherwise:
         *   - Asserts the kill command fails with Unauthorized.
         *   - Confirms the cursor is still alive via a follow-up getMore (expects success).
         *   - Cleans up the surviving cursor via adminConn.
         */
        function tryKill({openerConn, cursorDb, cursorColl, killerConn, requestDb, requestColl, expectKilled}) {
            const openerDb = openerConn.getDB(cursorDb);
            const killerDb = killerConn.getDB(requestDb);

            const openRes = assert.commandWorked(openerDb.runCommand({find: cursorColl, batchSize: 2}));
            const cursorId = openRes.cursor.id;
            assert.neq(cursorId, 0, "cursor.id should be non-zero; need more data in collection", {openRes});

            const killRes = killerDb.runCommand({killCursors: requestColl, cursors: [cursorId]});

            if (expectKilled) {
                assert.commandWorked(killRes, "expected killCursors to succeed", {killRes});
                // Confirm the cursor is actually gone.
                assert.commandFailedWithCode(
                    openerDb.runCommand({getMore: cursorId, collection: cursorColl}),
                    ErrorCodes.CursorNotFound,
                    "expected cursor to be gone after a successful kill",
                );
            } else {
                assert.commandFailedWithCode(
                    killRes,
                    ErrorCodes.Unauthorized,
                    "expected killCursors to be rejected with Unauthorized",
                    {killRes},
                );
                // Confirm the cursor is still alive.
                assert.commandWorked(
                    openerDb.runCommand({getMore: cursorId, collection: cursorColl}),
                    "expected cursor to still be alive after a rejected kill attempt",
                );
                // Clean up the surviving cursor via admin so it does not outlast the test.
                assert.commandWorked(
                    adminConn.getDB(cursorDb).runCommand({killCursors: cursorColl, cursors: [cursorId]}),
                );
            }
        }

        it("user can kill their own cursor with the honest namespace", function () {
            tryKill({
                openerConn: noPrivConn,
                cursorDb: "dbA",
                cursorColl: "collA1",
                killerConn: noPrivConn,
                requestDb: "dbA",
                requestColl: "collA1",
                expectKilled: true,
            });
        });

        it("user can kill their own cursor even when the request namespace is spoofed to another db", function () {
            // noPriv has no killAnyCursor privilege anywhere, but it still allowed to kill a cursor that they created.
            tryKill({
                openerConn: noPrivConn,
                cursorDb: "dbA",
                cursorColl: "collA1",
                killerConn: noPrivConn,
                requestDb: "dbB",
                requestColl: "collB1",
                expectKilled: true,
            });
        });

        it("db-level killAnyCursor on dbA kills alice's cursor on dbA.collA1", function () {
            tryKill({
                openerConn: aliceConn,
                cursorDb: "dbA",
                cursorColl: "collA1",
                killerConn: dbAKillerConn,
                requestDb: "dbA",
                requestColl: "collA1",
                expectKilled: true,
            });
        });

        it("db-level killAnyCursor on dbA kills alice's cursor on dbA.collA2 (grant covers all collections in the db)", function () {
            tryKill({
                openerConn: aliceConn,
                cursorDb: "dbA",
                cursorColl: "collA2",
                killerConn: dbAKillerConn,
                requestDb: "dbA",
                requestColl: "collA2",
                expectKilled: true,
            });
        });

        it("db-level killAnyCursor on dbA cannot kill bob's cursor on dbB (honest request namespace)", function () {
            tryKill({
                openerConn: bobConn,
                cursorDb: "dbB",
                cursorColl: "collB1",
                killerConn: dbAKillerConn,
                requestDb: "dbB",
                requestColl: "collB1",
                expectKilled: false,
            });
        });

        it("db-level killAnyCursor on dbA cannot kill bob's cursor on dbB via request namespace spoofing (SERVER-128198 cross-db repro)", function () {
            tryKill({
                openerConn: bobConn,
                cursorDb: "dbB",
                cursorColl: "collB1",
                killerConn: dbAKillerConn,
                requestDb: "dbA",
                requestColl: "collA1",
                expectKilled: false,
            });
        });

        it("collection-level killAnyCursor on dbA.collA1 kills alice's cursor on dbA.collA1", function () {
            tryKill({
                openerConn: aliceConn,
                cursorDb: "dbA",
                cursorColl: "collA1",
                killerConn: collA1KillerConn,
                requestDb: "dbA",
                requestColl: "collA1",
                expectKilled: true,
            });
        });

        it("collection-level killAnyCursor on dbA.collA1 cannot kill alice's cursor on dbA.collA2 (honest request namespace)", function () {
            // The grant is scoped to collA1; it must not cover sibling collection collA2.
            tryKill({
                openerConn: aliceConn,
                cursorDb: "dbA",
                cursorColl: "collA2",
                killerConn: collA1KillerConn,
                requestDb: "dbA",
                requestColl: "collA2",
                expectKilled: false,
            });
        });

        it("collection-level killAnyCursor on dbA.collA1 cannot kill alice's cursor on dbA.collA2 via request namespace spoofing (SERVER-128198 cross-collection repro)", function () {
            tryKill({
                openerConn: aliceConn,
                cursorDb: "dbA",
                cursorColl: "collA2",
                killerConn: collA1KillerConn,
                requestDb: "dbA",
                requestColl: "collA1",
                expectKilled: false,
            });
        });

        it("cluster-level killAnyCursor kills any cursor regardless of request namespace", function () {
            tryKill({
                openerConn: bobConn,
                cursorDb: "dbB",
                cursorColl: "collB1",
                killerConn: clusterKillerConn,
                requestDb: "dbA",
                requestColl: "collA1",
                expectKilled: true,
            });
        });

        it("db-level killAnyCursor on dbA kills alice's cursor on dbA even when the request namespace points to dbB", function () {
            tryKill({
                openerConn: aliceConn,
                cursorDb: "dbA",
                cursorColl: "collA1",
                killerConn: dbAKillerConn,
                requestDb: "dbB",
                requestColl: "collB1",
                expectKilled: true,
            });
        });

        it("user with only read privilege cannot kill another user's cursor", function () {
            tryKill({
                openerConn: aliceConn,
                cursorDb: "dbA",
                cursorColl: "collA1",
                killerConn: noPrivConn,
                requestDb: "dbA",
                requestColl: "collA1",
                expectKilled: false,
            });
        });
    });
}

registerTests("standalone", () => {
    const mongod = MongoRunner.runMongod({auth: ""});
    return {conn: mongod, stop: () => MongoRunner.stopMongod(mongod)};
});

registerTests("sharded", () => {
    const st = new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: "jstests/libs/key1"}});
    return {conn: st.s0, stop: () => st.stop()};
});
