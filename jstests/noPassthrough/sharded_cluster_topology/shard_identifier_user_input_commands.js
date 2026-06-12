/**
 * Exercises user-facing commands that accept a shard identifier, using shardId, shard URL
 * (connection string), and HostAndPort forms.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// commitShardRemoval can leave docs in config.rangeDeletions in state "pending".
TestData.skipCheckOrphans = true;

const identifierTypes = ["shardId", "shardURL", "hostAndPort"];

const dbName = jsTestName();
const shardedCollName = "sharded";
const unshardedCollName = "unsharded";
const reshardCollName = "reshard";
const shardedNs = dbName + "." + shardedCollName;
const unshardedCollNs = dbName + "." + unshardedCollName;
const reshardCollNs = dbName + "." + reshardCollName;
const zoneName = "zone_name";
let enableShardingDbCounter = 0;
let shardToUse = null;
let shardIdentifierForms = [];

function createShardToUse() {
    if (shardToUse) {
        shardToUse.stopSet({skipValidation: true});
        shardToUse = null;
    }
    shardToUse = new ReplSetTest({name: "shardToUse", nodes: 1});
    shardToUse.startSet({shardsvr: ""});
    shardToUse.initiate();
    assert.commandWorked(st.s.adminCommand({addShard: shardToUse.getURL()}));
    shardIdentifierForms = [
        {
            shardId: "shardToUse",
            shardURL: shardToUse.getURL(),
            hostAndPort: shardToUse.getPrimary().host,
        },
        {
            shardId: st.shard0.shardName,
            shardURL: st.shard0.host,
            hostAndPort: st.rs0.getPrimary().host,
        },
    ];
}

const testCommands = {
    addShardToZone: {
        commandToRun: (identifier) => ({addShardToZone: identifier, zone: zoneName}),
        runPreconditions: () => {},
        runPostconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(
                conn.runCommand({removeShardFromZone: sameShardId, zone: zoneName}),
            );
        },
        expectedOutcomes: {
            shardId: true,
            shardURL: false,
            hostAndPort: false,
        },
    },

    removeShardFromZone: {
        commandToRun: (identifier) => ({removeShardFromZone: identifier, zone: zoneName}),
        runPreconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(conn.runCommand({addShardToZone: sameShardId, zone: zoneName}));
        },
        runPostconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(conn.runCommand({addShardToZone: sameShardId, zone: zoneName}));
        },
        expectedOutcomes: {
            shardId: true,
            shardURL: false,
            hostAndPort: false,
        },
    },

    enableSharding: {
        commandToRun: (identifier) => ({
            enableSharding: dbName + "_es" + enableShardingDbCounter++,
            primaryShard: identifier,
        }),
        runPreconditions: () => {},
        runPostconditions: () => {},
        expectedOutcomes: {
            shardId: true,
            shardURL: true,
            hostAndPort: true,
        },
    },

    moveChunk: {
        commandToRun: (identifier) => ({
            moveChunk: shardedNs,
            find: {_id: 0},
            to: identifier,
        }),
        runPreconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(
                conn.runCommand({moveRange: shardedNs, min: {_id: MinKey}, toShard: otherShardId}),
            );
        },
        runPostconditions: () => {},
        expectedOutcomes: {
            shardId: true,
            shardURL: true,
            hostAndPort: true,
        },
    },

    moveRange: {
        commandToRun: (identifier) => ({
            moveRange: shardedNs,
            min: {_id: MinKey},
            toShard: identifier,
        }),
        runPreconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(
                conn.runCommand({moveRange: shardedNs, min: {_id: MinKey}, toShard: otherShardId}),
            );
        },
        runPostconditions: () => {},
        expectedOutcomes: {
            shardId: true,
            shardURL: true,
            hostAndPort: true,
        },
    },

    movePrimary: {
        commandToRun: (identifier) => ({movePrimary: dbName, to: identifier}),
        runPreconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(conn.runCommand({movePrimary: dbName, to: otherShardId}));
        },
        runPostconditions: () => {},
        expectedOutcomes: {
            shardId: true,
            shardURL: true,
            hostAndPort: true,
        },
    },

    mergeAllChunksOnShard: {
        commandToRun: (identifier) => ({
            mergeAllChunksOnShard: shardedNs,
            shard: identifier,
        }),
        runPreconditions: () => {},
        runPostconditions: () => {},
        expectedOutcomes: {
            shardId: true,
            shardURL: true,
            hostAndPort: true,
        },
    },

    killOp: {
        commandToRun: (identifier) => ({killOp: 1, op: identifier + ":1"}),
        runPreconditions: () => {},
        runPostconditions: () => {},
        expectedOutcomes: {
            shardId: true,
            shardURL: false,
            hostAndPort: false,
        },
    },

    commitShardRemoval: {
        commandToRun: (identifier) => ({commitShardRemoval: identifier}),
        runPreconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(conn.runCommand({startShardDraining: sameShardId}));
            assert.commandWorked(conn.runCommand({balancerStart: 1}));
            let dbDocs = conn
                .getSiblingDB("config")
                .databases.find({primary: sameShardId})
                .toArray();
            for (const dbDoc of dbDocs) {
                assert.commandWorked(conn.runCommand({movePrimary: dbDoc._id, to: otherShardId}));
            }
            assert.soon(
                () => {
                    let res = conn.runCommand({shardDrainingStatus: sameShardId});
                    return res.ok && res.state == "drainingComplete";
                },
                "shard " + sameShardId + " did not finish draining before commitShardRemoval",
            );
        },
        runPostconditions: (conn, sameShardId, otherShardId) => {
            createShardToUse();
        },
        expectedOutcomes: {
            shardId: true,
            shardURL: true,
            hostAndPort: true,
        },
    },

    unshardCollection: {
        commandToRun: (identifier) => ({unshardCollection: unshardedCollNs, toShard: identifier}),
        runPreconditions: (conn) => {
            assert.commandWorked(
                conn.runCommand({shardCollection: unshardedCollNs, key: {_id: 1}}),
            );
        },
        runPostconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(
                conn.runCommand({unshardCollection: unshardedCollNs, toShard: sameShardId}),
            );
        },
        expectedOutcomes: {
            shardId: true,
            shardURL: false,
            hostAndPort: false,
        },
    },

    moveCollection: {
        commandToRun: (identifier) => ({moveCollection: unshardedCollNs, toShard: identifier}),
        runPreconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(
                conn.runCommand({moveCollection: unshardedCollNs, toShard: otherShardId}),
            );
        },
        runPostconditions: () => {},
        expectedOutcomes: {
            shardId: true,
            shardURL: false,
            hostAndPort: false,
        },
    },

    reshardCollection: {
        commandToRun: (identifier) => ({
            reshardCollection: reshardCollNs,
            key: {_id: 1},
            forceRedistribution: true,
            shardDistribution: [{shard: identifier, min: {_id: MinKey}, max: {_id: MaxKey}}],
        }),
        runPreconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(
                conn.runCommand({
                    moveChunk: reshardCollNs,
                    find: {_id: 0},
                    to: otherShardId,
                    _waitForDelete: true,
                }),
            );
        },
        runPostconditions: () => {},
        expectedOutcomes: {
            shardId: true,
            shardURL: false,
            hostAndPort: false,
        },
    },

    shardDrainingStatus: {
        commandToRun: (identifier) => ({shardDrainingStatus: identifier}),
        runPreconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(conn.runCommand({startShardDraining: sameShardId}));
        },
        runPostconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(conn.runCommand({stopShardDraining: sameShardId}));
        },
        expectedOutcomes: {
            shardId: true,
            shardURL: true,
            hostAndPort: true,
        },
    },

    removeShard: {
        commandToRun: (identifier) => ({removeShard: identifier}),
        runPreconditions: () => {},
        runPostconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorkedOrFailedWithCode(
                conn.runCommand({stopShardDraining: sameShardId}),
                ErrorCodes.IllegalOperation,
            );
        },
        expectedOutcomes: {
            shardId: true,
            shardURL: true,
            hostAndPort: true,
        },
    },

    stopShardDraining: {
        commandToRun: (identifier) => ({stopShardDraining: identifier}),
        runPreconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorked(conn.runCommand({startShardDraining: sameShardId}));
        },
        runPostconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorkedOrFailedWithCode(
                conn.runCommand({stopShardDraining: sameShardId}),
                ErrorCodes.IllegalOperation,
            );
        },
        expectedOutcomes: {
            shardId: true,
            shardURL: true,
            hostAndPort: true,
        },
    },

    startShardDraining: {
        commandToRun: (identifier) => ({startShardDraining: identifier}),
        runPreconditions: () => {},
        runPostconditions: (conn, sameShardId, otherShardId) => {
            assert.commandWorkedOrFailedWithCode(
                conn.runCommand({startShardDraining: sameShardId}),
                ErrorCodes.IllegalOperation,
            );
        },
        expectedOutcomes: {
            shardId: true,
            shardURL: true,
            hostAndPort: true,
        },
    },
};

const st = new ShardingTest({
    name: "shard_identifier_user_input_commands",
    shards: 1,
    mongos: 1,
});
createShardToUse();
const adminDB = st.s.getDB("admin");
const mongosDB = st.s.getDB(dbName);

assert.commandWorked(
    adminDB.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(adminDB.runCommand({shardCollection: shardedNs, key: {_id: 1}}));
assert.commandWorked(adminDB.runCommand({shardCollection: reshardCollNs, key: {_id: 1}}));
assert.commandWorked(mongosDB[shardedCollName].insert({_id: 0}));
assert.commandWorked(mongosDB[reshardCollName].insert({_id: 0}));
assert.commandWorked(mongosDB.createCollection(unshardedCollName));

for (const command of Object.keys(testCommands)) {
    for (const identifierType of identifierTypes) {
        jsTest.log.info(
            "Testing command: " +
                command +
                " with shard identifier form: " +
                tojson(identifierType),
        );
        testCommands[command].runPreconditions(
            adminDB,
            shardIdentifierForms[0].shardId,
            shardIdentifierForms[1].shardId,
        );
        let res = adminDB.runCommand(
            testCommands[command].commandToRun(shardIdentifierForms[0][identifierType]),
        );
        if (testCommands[command].expectedOutcomes[identifierType] === true) {
            assert.commandWorked(res);
        } else {
            assert.commandFailed(res);
        }
        testCommands[command].runPostconditions(
            adminDB,
            shardIdentifierForms[0].shardId,
            shardIdentifierForms[1].shardId,
        );
    }
}

st.stop();
if (shardToUse) {
    shardToUse.stopSet({skipValidation: true});
    shardToUse = null;
}
