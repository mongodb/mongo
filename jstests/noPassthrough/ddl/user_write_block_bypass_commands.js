/**
 * Verifies that a privileged user with write-block bypass can run DDL commands on a sharded
 * cluster while global user write blocking is enabled.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_auth,
 * ]
 */
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const keyFile = "jstests/libs/key1";

// User that can both toggle write blocking and bypass it (mongosync-style privileges).
const bypassUser = "bypassUser";
const password = "password";

function enableShardedDb(st, dbName) {
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
    );
}

function setupShardedCollection(st, dbName, collName = "sharded") {
    enableShardedDb(st, dbName);
    const nss = `${dbName}.${collName}`;
    const db = st.s.getDB(dbName);

    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: nss, middle: {_id: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: st.shard1.shardName}),
    );

    assert.commandWorked(db.getCollection(collName).insert({_id: -1, shard: "shard0"}));
    assert.commandWorked(db.getCollection(collName).insert({_id: 1, shard: "shard1"}));

    return {collName, nss};
}

function setHistoryWindowInSecs(st, valueInSeconds) {
    for (const rs of [st.configRS, st.rs0, st.rs1]) {
        configureFailPointForRS(
            rs.nodes,
            "overrideHistoryWindowInSecs",
            {seconds: valueInSeconds},
            "alwaysOn",
        );
    }
}

function resetHistoryWindowInSecs(st) {
    for (const rs of [st.configRS, st.rs0, st.rs1]) {
        configureFailPointForRS(rs.nodes, "overrideHistoryWindowInSecs", {}, "off");
    }
}

function setUserWriteBlockMode(mongosAdmin, enabled) {
    assert.commandWorked(mongosAdmin.runCommand({setUserWriteBlockMode: 1, global: enabled}));
}

function runCommandWithMaxTimeMS(st, mongosAdmin, targetDbName, command) {
    jsTest.log.info("Enabling user write blocking");
    setUserWriteBlockMode(mongosAdmin, true);

    try {
        let commandWithMaxTimeMs = Object.extend(command, {maxTimeMS: 60 * 1000});
        assert.commandWorked(st.s.getDB(targetDbName).runCommand(commandWithMaxTimeMs));
    } finally {
        jsTest.log.info("Disabling user write blocking");
        setUserWriteBlockMode(mongosAdmin, false);
    }
}

const testCases = [
    {
        name: "dropDatabase",
        targetDbName(ctx) {
            return ctx.dbName;
        },
        setup(st, ctx) {
            setupShardedCollection(st, ctx.dbName);
        },
        command() {
            return {dropDatabase: 1};
        },
    },
    {
        name: "dropCollection",
        targetDbName(ctx) {
            return ctx.dbName;
        },
        setup(st, ctx) {
            const {collName} = setupShardedCollection(st, ctx.dbName);
            ctx.collName = collName;
        },
        command(ctx) {
            return {drop: ctx.collName};
        },
    },
    {
        name: "shardCollection",
        targetDbName() {
            return "admin";
        },
        setup(st, ctx) {
            enableShardedDb(st, ctx.dbName);
            ctx.collName = "toShard";
            ctx.nss = `${ctx.dbName}.${ctx.collName}`;
            assert.commandWorked(st.s.getDB(ctx.dbName).createCollection(ctx.collName));
        },
        command(ctx) {
            return {
                shardCollection: ctx.nss,
                key: {_id: 1},
            };
        },
    },
    {
        name: "collMod",
        targetDbName(ctx) {
            return ctx.dbName;
        },
        setup(st, ctx) {
            const {collName} = setupShardedCollection(st, ctx.dbName);
            ctx.collName = collName;
            assert.commandWorked(
                st.s.getDB(ctx.dbName).getCollection(collName).createIndex({a: 1}),
            );
        },
        command(ctx) {
            return {
                collMod: ctx.collName,
                index: {name: "a_1", hidden: true},
            };
        },
    },
    {
        name: "renameCollection",
        targetDbName() {
            return "admin";
        },
        setup(st, ctx) {
            const {nss: sourceNss} = setupShardedCollection(st, ctx.dbName, "source");
            ctx.sourceNss = sourceNss;
            ctx.targetNss = `${ctx.dbName}.target`;

            const targetColl = st.s.getDB(ctx.dbName).target;
            assert.commandWorked(st.s.adminCommand({shardCollection: ctx.targetNss, key: {x: 1}}));
            assert.commandWorked(targetColl.insert({x: 0, value: "old-target-0"}));
            assert.commandWorked(targetColl.insert({x: 2, value: "old-target-2"}));
            assert.commandWorked(st.s.adminCommand({split: ctx.targetNss, middle: {x: 1}}));
            assert.commandWorked(
                st.s.adminCommand({
                    moveChunk: ctx.targetNss,
                    find: {x: 2},
                    to: st.shard1.shardName,
                }),
            );
        },
        command(ctx) {
            return {
                renameCollection: ctx.sourceNss,
                to: ctx.targetNss,
                dropTarget: true,
            };
        },
    },
    {
        name: "dropIndexes",
        targetDbName(ctx) {
            return ctx.dbName;
        },
        setup(st, ctx) {
            const {collName} = setupShardedCollection(st, ctx.dbName);
            ctx.collName = collName;
            assert.commandWorked(
                st.s.getDB(ctx.dbName).getCollection(collName).createIndex({a: 1}),
            );
        },
        command(ctx) {
            return {
                dropIndexes: ctx.collName,
                index: "a_1",
            };
        },
    },
    {
        name: "refineCollectionShardKey",
        targetDbName() {
            return "admin";
        },
        setup(st, ctx) {
            enableShardedDb(st, ctx.dbName);
            ctx.collName = "sharded";
            ctx.nss = `${ctx.dbName}.${ctx.collName}`;
            const coll = st.s.getDB(ctx.dbName).getCollection(ctx.collName);

            assert.commandWorked(st.s.adminCommand({shardCollection: ctx.nss, key: {a: 1}}));
            assert.commandWorked(coll.createIndex({a: 1, b: 1}));
            assert.commandWorked(coll.insert({a: 1, b: 1}));
        },
        command(ctx) {
            return {
                refineCollectionShardKey: ctx.nss,
                key: {a: 1, b: 1},
            };
        },
    },
    {
        name: "movePrimary",
        targetDbName() {
            return "admin";
        },
        setup(st, ctx) {
            enableShardedDb(st, ctx.dbName);
            const collName = "unsharded";
            const coll = st.s.getDB(ctx.dbName).getCollection(collName);
            assert.commandWorked(coll.createIndex({x: 1}));
            assert.commandWorked(coll.insert({x: 1}));
        },
        command(ctx) {
            return {
                movePrimary: ctx.dbName,
                to: ctx.toShard,
            };
        },
    },
    {
        name: "split",
        targetDbName() {
            return "admin";
        },
        setup(st, ctx) {
            enableShardedDb(st, ctx.dbName);
            ctx.collName = "sharded";
            ctx.nss = `${ctx.dbName}.${ctx.collName}`;

            assert.commandWorked(st.s.adminCommand({shardCollection: ctx.nss, key: {_id: 1}}));
            assert.commandWorked(
                st.s.getDB(ctx.dbName).getCollection(ctx.collName).insert({_id: 0}),
            );
        },
        command(ctx) {
            return {
                split: ctx.nss,
                middle: {_id: 0},
            };
        },
    },
    {
        name: "mergeChunks",
        targetDbName() {
            return "admin";
        },
        setup(st, ctx) {
            enableShardedDb(st, ctx.dbName);
            ctx.nss = `${ctx.dbName}.sharded`;

            assert.commandWorked(st.s.adminCommand({shardCollection: ctx.nss, key: {x: 1}}));
            for (const middle of [-1, 0, 1]) {
                assert.commandWorked(st.s.adminCommand({split: ctx.nss, middle: {x: middle}}));
            }
            ctx.mergeBounds = [{x: -1}, {x: 1}];
        },
        command(ctx) {
            return {
                mergeChunks: ctx.nss,
                bounds: ctx.mergeBounds,
            };
        },
    },
    {
        name: "moveChunk",
        targetDbName() {
            return "admin";
        },
        setup(st, ctx) {
            enableShardedDb(st, ctx.dbName);
            ctx.nss = `${ctx.dbName}.sharded`;
            ctx.toShard = st.shard1.shardName;

            assert.commandWorked(st.s.adminCommand({shardCollection: ctx.nss, key: {_id: 1}}));
            assert.commandWorked(st.s.adminCommand({split: ctx.nss, middle: {_id: 0}}));
            assert.commandWorked(st.s.getDB(ctx.dbName).sharded.insert({_id: 0}));
        },
        command(ctx) {
            return {
                moveChunk: ctx.nss,
                find: {_id: 0},
                to: ctx.toShard,
            };
        },
    },
    {
        name: "moveRange",
        targetDbName() {
            return "admin";
        },
        setup(st, ctx) {
            enableShardedDb(st, ctx.dbName);
            ctx.nss = `${ctx.dbName}.coll`;
            ctx.toShard = st.shard1.shardName;

            assert.commandWorked(st.s.adminCommand({shardCollection: ctx.nss, key: {x: 1}}));
            assert.commandWorked(st.s.adminCommand({split: ctx.nss, middle: {x: 0}}));
            assert.commandWorked(st.s.getDB(ctx.dbName).coll.insert({x: 0}));
        },
        command(ctx) {
            return {
                moveRange: ctx.nss,
                min: {x: 0},
                toShard: ctx.toShard,
            };
        },
    },
    {
        name: "setAllowMigrations",
        targetDbName() {
            return "admin";
        },
        setup(st, ctx) {
            enableShardedDb(st, ctx.dbName);
            ctx.nss = `${ctx.dbName}.coll`;

            assert.commandWorked(st.s.adminCommand({shardCollection: ctx.nss, key: {_id: 1}}));
            assert.commandWorked(st.s.adminCommand({split: ctx.nss, middle: {_id: 0}}));
            assert.commandWorked(st.s.getDB(ctx.dbName).coll.insert({_id: 0}));
        },
        command(ctx) {
            return {setAllowMigrations: ctx.nss, allowMigrations: false};
        },
    },
    {
        name: "mergeAllChunksOnShard",
        targetDbName() {
            return "admin";
        },
        setup(st, ctx) {
            enableShardedDb(st, ctx.dbName);
            ctx.nss = `${ctx.dbName}.coll`;
            ctx.shardName = st.shard0.shardName;

            assert.commandWorked(st.s.adminCommand({shardCollection: ctx.nss, key: {x: 1}}));
            for (const middle of [10, 20, 30]) {
                assert.commandWorked(st.s.adminCommand({split: ctx.nss, middle: {x: middle}}));
            }
            setHistoryWindowInSecs(st, -10 * 60);
        },
        command(ctx) {
            return {
                mergeAllChunksOnShard: ctx.nss,
                shard: ctx.shardName,
            };
        },
    },
];

describe("user write block bypass commands on sharded cluster", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 2,
            rs: {nodes: 1},
            other: {keyFile: keyFile},
        });

        const mongosAdmin = this.st.s.getDB("admin");
        assert.commandWorked(
            mongosAdmin.runCommand({
                createUser: bypassUser,
                pwd: password,
                roles: [
                    {role: "root", db: "admin"},
                    {role: "restore", db: "admin"},
                ],
            }),
        );
        assert(mongosAdmin.auth(bypassUser, password));

        this.mongosAdmin = mongosAdmin;
    });

    afterEach(function () {
        // Ensure write blocking is off if a test fails while blocked.
        setUserWriteBlockMode(this.mongosAdmin, false);
        resetHistoryWindowInSecs(this.st);
    });

    after(function () {
        this.st.stop();
    });

    for (const testCase of testCases) {
        it(`'${testCase.name}' succeeds while user writes are blocked`, function () {
            const ctx = {
                dbName: jsTestName() + "_" + testCase.name,
                toShard: this.st.shard1.shardName,
            };
            jsTest.log.info("Setting up test database", {
                dbName: ctx.dbName,
                command: testCase.name,
            });
            testCase.setup(this.st, ctx);

            const targetDbName = testCase.targetDbName ? testCase.targetDbName(ctx) : "admin";
            jsTest.log.info("Running command while user writes are blocked", {
                targetDbName,
                command: testCase.name,
            });
            runCommandWithMaxTimeMS(this.st, this.mongosAdmin, targetDbName, testCase.command(ctx));
        });
    }
});
