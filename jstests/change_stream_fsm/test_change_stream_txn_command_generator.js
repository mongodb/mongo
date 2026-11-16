/**
 * Unit-ish tests for the EOT txn command generator, txn commands, and matchers.
 * Same role as test_change_stream_sharding_command_generator.js for the sharding model.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   incompatible_aubsan,
 *   tsan_incompatible,
 *   uses_change_streams,
 * ]
 */
import {TxnContext} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {
    TxnModelReplicaSet,
    TxnModelShardedCluster,
} from "jstests/libs/util/change_stream/change_stream_txn_test_model.js";
import {
    TxnCommandGenerator,
    TxnCommandGeneratorParams,
} from "jstests/libs/util/change_stream/change_stream_txn_command_generator.js";
import {
    AbortTxnCommand,
    BeginTxnCommand,
    CommitTxnCommand,
    TxnOpCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {TEST_SEED} from "jstests/libs/util/change_stream/change_stream_sharding_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {Writer} from "jstests/libs/util/change_stream/change_stream_writer.js";
import {describe, it} from "jstests/libs/mochalite.js";

// TxnOpCommands ride the session; in the flat walk list their events flow through
// their CommitTxnCommand envelope (the txn's assertion unit).
function flattenTxnOps(commands) {
    return commands.filter((c) => c instanceof CommitTxnCommand).flatMap((c) => c.commandsInTxn);
}

jsTest.log.info(`test_change_stream_txn_command_generator: using seed ${TEST_SEED}`);

// Generation is pure computation — no cluster needed. The sharded params exist even in
// Phase A (the sharded graph is generated, just not executed until Phase B). collTxn is
// the one collection whose SHARDEDNESS is a topology knob: sharded on a multi-shard
// cluster (spanning every shard — the presplit covers the full key space), an ordinary
// unsharded collection on a replica set.
//
// Two unsharded collections per (db, shard): a NEW_COLL_SAME_DB_SAME_SHARD edge that
// UPGRADES the scope (from a ONE_COLL state) must land on an untouched collection on
// the SAME shard, so one touched coll must never exhaust a (db, shard) slot; and every
// shard hosts db2 collections so NEW_*_NEW_SHARD edges always have a fresh candidate.
// collTxn counts as a db1 candidate on every shard.
// The suite configuration (the topology) picks the model: each topology's model
// carries its own state class and graph. The generator consumes the walk contract
// (TransactionTestModelBase) without knowing which topology it drives.
function makeModel(topology) {
    return topology === "sharded" ? new TxnModelShardedCluster() : new TxnModelReplicaSet();
}

function makeParams(topology) {
    const sharded = topology === "sharded";
    const shardIds = sharded ? ["s0", "s1", "s2"] : ["rs0"];
    const colls = [
        {
            ns: {db: "txn_fsm_db1", coll: "collTxn"},
            placementShards: [...shardIds],
            shardedConfig: sharded ? {shardKey: {data: 1}, rangeWidth: 100000} : null,
        },
    ];
    for (const [tag, dbName] of [
        ["D1", "txn_fsm_db1"],
        ["D2", "txn_fsm_db2"],
    ]) {
        shardIds.forEach((shardId, i) => {
            for (const suffix of ["a", "b"]) {
                colls.push({
                    ns: {db: dbName, coll: `coll${tag}${i}${suffix}`},
                    placementShards: [shardId],
                    shardedConfig: null,
                });
            }
        });
    }
    return new TxnCommandGeneratorParams({
        colls,
    });
}

describe("Txn command generator (pure generation, both topologies)", function () {
    for (const topology of ["rs", "sharded"]) {
        it(`generates identical command sequences for the same seed (${topology})`, () => {
            const params = makeParams(topology);
            const model = makeModel(topology);
            const gen1 = new TxnCommandGenerator(TEST_SEED);
            const gen2 = new TxnCommandGenerator(TEST_SEED);
            const c1 = gen1.generateCommands(model, params);
            const c2 = gen2.generateCommands(model, params);
            assert.eq(c1.length, c2.length, "Same seed must produce the same number of commands");
            for (let i = 0; i < c1.length; i++) {
                assert.eq(c1[i].toString(), c2[i].toString(), `Command ${i} differs`);
            }
        });

        it(`covers every command kind and ends at TXN_IDLE via commit (${topology})`, () => {
            const params = makeParams(topology);
            const model = makeModel(topology);
            const commands = new TxnCommandGenerator(TEST_SEED).generateCommands(model, params);
            const all = [...commands, ...flattenTxnOps(commands)];
            const seen = new Set(all.map((c) => c.constructor.name));
            for (const name of [
                "BeginTxnCommand",
                "TxnOpCommand",
                "CommitTxnCommand",
                "AbortTxnCommand",
                "InsertDocCommand",
            ]) {
                assert(seen.has(name), `Expected ${name} in the generated sequence`);
            }
            // The sequence must leave NO txn open: track the txn state through the
            // flat list — BEGIN opens it, statement ops ride it, COMMIT/ABORT close
            // it. A walk that ends mid-txn (a missing walk-end closure) fails here.
            let open = false;
            for (const cmd of commands) {
                if (cmd instanceof BeginTxnCommand) {
                    assert(!open, "BEGIN inside an open txn");
                    open = true;
                } else if (cmd instanceof CommitTxnCommand || cmd instanceof AbortTxnCommand) {
                    assert(open, "COMMIT/ABORT outside an open txn");
                    open = false;
                }
            }
            assert(!open, "The walk ended with an open (uncommitted) txn");
        });

        it("round-trips CommitTxnCommand through toSpec/fromSpec with nested children", () => {
            const params = makeParams(topology);
            const model = makeModel(topology);
            const commands = new TxnCommandGenerator(TEST_SEED).generateCommands(model, params);
            const commit = commands.find(
                (c) => c instanceof CommitTxnCommand && c.commandsInTxn.length > 0,
            );
            assert(commit, "No CommitTxnCommand generated");
            const spec = commit.toSpec();
            const restored = CommitTxnCommand.fromSpec(spec);
            assert.eq(
                restored.commandsInTxn.length,
                commit.commandsInTxn.length,
                "Nested children lost in spec round-trip",
            );
            assert.eq(
                restored.getChangeEvents("collection"),
                commit.getChangeEvents("collection"),
                "Prediction changed in spec round-trip",
            );
        });
    }
});

describe("Txn commands: outcome gating and envelope shape", function () {
    it("outcome-gates TxnOpCommand event prediction", () => {
        const params = makeParams("rs");
        const model = makeModel("rs");
        const commands = new TxnCommandGenerator(TEST_SEED).generateCommands(model, params);
        const op = flattenTxnOps(commands).find((c) => c instanceof TxnOpCommand);
        assert(op, "No TxnOpCommand generated");
        assert.eq(
            op.getChangeEvents("collection", TxnContext.kInProgress).length,
            0,
            "inProgress must predict nothing",
        );
        assert.eq(
            op.getChangeEvents("collection", TxnContext.kAborted).length,
            0,
            "aborted must predict nothing",
        );
        assert.gt(
            op.getChangeEvents("collection", TxnContext.kCommitted).length,
            0,
            "committed must predict its insert",
        );
    });
});

describe("Txn execution on a replica set (Phase A)", function () {
    it("executes a generated walk through a session-holding writer", function () {
        const rst = new ReplSetTest({name: "txn_exec_rs", nodes: 3});
        rst.startSet();
        rst.initiate();
        try {
            const primary = rst.getPrimary();
            const params = makeParams("rs");
            for (const c of params.colls) {
                assert.commandWorked(primary.getDB(c.ns.db).createCollection(c.ns.coll));
            }

            const model = makeModel("rs");
            const commands = new TxnCommandGenerator(TEST_SEED).generateCommands(model, params);
            // The REAL writer machinery: the walk crosses the Thread boundary, the
            // thread holds one session ({holdSession: true}), and thread failures
            // propagate through joinAll() — asserted before fixture teardown.
            Writer.run(primary, "txn_writer_thread", commands, TEST_SEED, {holdSession: true});
            Writer.joinAll();
            // The walk's committed writes landed somewhere in the fixture (which
            // collections depends on the walk's random picks; ABORT_TXN edges roll
            // theirs back).
            const totalDocs = params.colls.reduce(
                (n, c) => n + primary.getDB(c.ns.db)[c.ns.coll].countDocuments({}),
                0,
            );
            assert.gt(totalDocs, 0, "The walk's committed writes landed");
        } finally {
            rst.stopSet();
        }
    });
});
