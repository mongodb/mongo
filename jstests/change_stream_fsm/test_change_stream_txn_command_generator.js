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
    CommittedTxn,
    CreateIndexCommand,
    InsertDocCommand,
    TxnOpCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";
import {
    ChangeEventMatcher,
    TxnChangeEventMatcher,
    EotMode,
} from "jstests/libs/util/change_stream/change_stream_event.js";
import {SingleChangeStreamMatcher} from "jstests/libs/util/change_stream/change_stream_matcher.js";
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
                assert(seen.has(name), `Expected ${name} in the generated sequence`, {
                    generatedKinds: [...seen],
                });
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
            // The prediction is either a CommittedTxn envelope or a bare list (an
            // event-less txn predicts no envelope) — assert the WHOLE thing: the
            // restored command's prediction must equal the original's, shape included.
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
            {op: op.toString()},
        );
        assert.eq(
            op.getChangeEvents("collection", TxnContext.kAborted).length,
            0,
            "aborted must predict nothing",
            {op: op.toString()},
        );
        assert.gt(
            op.getChangeEvents("collection", TxnContext.kCommitted).length,
            0,
            "committed must predict its insert",
            {op: op.toString()},
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
            assert.gt(totalDocs, 0, "The walk's committed writes landed", {totalDocs});
        } finally {
            rst.stopSet();
        }
    });
});

describe("Pool matcher + stream-level dedup (pure logic, no cluster)", function () {
    it("drains the to-be-matched set and gates EOT on set-empty", () => {
        const committedTxn = new CommitTxnCommand({
            commandsInTxn: [
                new InsertDocCommand({
                    dbName: "db",
                    collName: "coll",
                    collectionCtx: {exists: true},
                    documents: [{a: 1}],
                }),
                new InsertDocCommand({
                    dbName: "db",
                    collName: "coll",
                    collectionCtx: {exists: true},
                    documents: [{a: 2}],
                }),
            ],
        }).getChangeEvents(ChangeStreamWatchMode.kCollection)[0];
        const matcher = new TxnChangeEventMatcher(committedTxn, {
            eotMode: EotMode.kRequired,
        });

        const [e1, e2, eot] = committedTxn.events;

        // The EOT refused while the to-be-matched set is non-empty.
        assert(
            !matcher.matches(eot, false),
            "EOT must be refused while the to-be-matched set is non-empty",
            {state: matcher.toString()},
        );
        assert(matcher.matches(e2, false), "Pool event 2 matches", {state: matcher.toString()});
        assert(matcher.matches(e1, false), "Pool event 1 matches (order-free)", {
            state: matcher.toString(),
        });
        assert(matcher.matches(eot, false), "Expected to match EOT event but failed", {
            state: matcher.toString(),
        });
        assert(matcher.isDone(), "Txn matcher must be exhausted after EOT");
        assert.eq(matcher.getMatchedCount(), 3, "Two statements + the EOT", {
            matchedCount: matcher.getMatchedCount(),
            state: matcher.toString(),
        });
    });

    it("binds the txn identity on first consumption and refuses foreign events", () => {
        const ev = (id, lsid, txnNumber) => ({
            operationType: "insert",
            ns: {db: "d", coll: "c"},
            fullDocument: {_id: id},
            lsid,
            txnNumber,
        });
        const eot = (lsid, txnNumber) => ({operationType: "endOfTransaction", lsid, txnNumber});
        const matcher = new TxnChangeEventMatcher(
            new CommittedTxn([ev("a", "L", NumberLong(0)), eot("L", NumberLong(0))]),
            {eotMode: EotMode.kRequired},
        );
        // The first consumed event binds the envelope's identity...
        assert(matcher.matches(ev("a", "L", NumberLong(0)), false), "Own statement matches");
        // ...a foreign transaction's EOT must not complete it (the EOT arm is
        // structural — without the identity check any EOT would match).
        assert(!matcher.matches(eot("OTHER", NumberLong(9)), false), "Foreign EOT refused");
        assert(!matcher.isDone(), "Still waiting for the own EOT");
        assert.eq(matcher.getTotalCount(), 2, "The refused event changed nothing");
        // The own EOT completes the envelope.
        assert(matcher.matches(eot("L", NumberLong(0)), false), "Own EOT matches");
        assert(matcher.isDone(), "Done after the own EOT");
    });

    it("keeps getTotalCount consumption-independent", () => {
        const e1 = {operationType: "insert", ns: {db: "d", coll: "c"}, fullDocument: {_id: "a"}};
        const eot = {operationType: "endOfTransaction", lsid: "L", txnNumber: NumberLong(1)};
        const matcher = new TxnChangeEventMatcher(new CommittedTxn([e1, eot]), {
            eotMode: EotMode.kRequired,
        });
        assert.eq(matcher.getTotalCount(), 2, "Before any consumption");
        assert(matcher.matches(e1, false), "Statement matches");
        assert.eq(matcher.getTotalCount(), 2, "Total is stable while the EOT is pending");
    });

    it("advances the stream through a txn envelope to the post-txn event", () => {
        const ev = (id) => ({
            operationType: "insert",
            ns: {db: "d", coll: "c"},
            fullDocument: {_id: id},
            lsid: "L",
            txnNumber: NumberLong(7),
        });
        const eot = {operationType: "endOfTransaction", lsid: "L", txnNumber: NumberLong(7)};
        // A post-txn AUTOCOMMIT event carries no txn identity (no lsid/txnNumber).
        const postTxn = {
            operationType: "insert",
            ns: {db: "d", coll: "c"},
            fullDocument: {_id: "z"},
        };
        const stream = new SingleChangeStreamMatcher([
            new TxnChangeEventMatcher(new CommittedTxn([ev("a"), eot]), {
                eotMode: EotMode.kRequired,
            }),
            new ChangeEventMatcher(postTxn),
        ]);

        assert(stream.matches(ev("a"), false), "Pool event matches", {
            breakdown: stream.getPerStreamBreakdown(),
        });
        assert(stream.matches(eot, false), "EOT matches after drain", {
            breakdown: stream.getPerStreamBreakdown(),
        });
        // The completed txn matcher is transparent: the stream advanced to the
        // autocommit event.
        assert(stream.matches(postTxn, false), "Post-txn event matches", {
            breakdown: stream.getPerStreamBreakdown(),
        });
        assert(stream.isDone(), "Stream done");
        assert.eq(stream.getMatchedCount(), 3, "Two statement events + the EOT");
    });

    it("completes without the EOT in whenAvailable mode and closes the envelope", () => {
        const e1 = {operationType: "insert", ns: {db: "d", coll: "c"}, fullDocument: {_id: "a"}};
        const eot = {operationType: "endOfTransaction", lsid: "L", txnNumber: NumberLong(1)};
        const matcher = new TxnChangeEventMatcher(new CommittedTxn([e1, eot]), {
            eotMode: EotMode.kWhenAvailable,
        });
        assert(matcher.matches(e1, false), "Statement matches", {state: matcher.toString()});
        // The pool drained — done WITHOUT the EOT (the verify loop cannot demand it:
        // whether the EOT ever arrives is unknowable to the matcher).
        assert(matcher.isDone(), "Done once the pool drains in whenAvailable mode");
        assert.eq(matcher.getMatchedCount(), 1, "Only the statement matched", {
            state: matcher.toString(),
        });
        // The envelope is closed: a late EOT is refused, counts frozen. In the wired
        // path the stream advanced past the done matcher, so a late EOT never gets here.
        assert(!matcher.matches(eot, false), "A done envelope is closed to the late EOT");
        assert.eq(matcher.getMatchedCount(), 1, "The refused EOT changed nothing");
    });

    it("runs with no EOT arm in off mode", () => {
        const e1 = {operationType: "insert", ns: {db: "d", coll: "c"}, fullDocument: {_id: "a"}};
        const eot = {operationType: "endOfTransaction", lsid: "L", txnNumber: NumberLong(1)};
        const matcher = new TxnChangeEventMatcher(new CommittedTxn([e1, eot]), {
            eotMode: EotMode.kOff,
        });
        assert.eq(matcher.getTotalCount(), 1, "The EOT arm is not part of the total");
        assert(matcher.matches(e1, false), "Statement matches");
        assert(matcher.isDone(), "Done once the pool drains");
        assert(!matcher.matches(eot, false), "No EOT arm to match");
        assert.eq(matcher.getMatchedCount(), 1, "Counts frozen after done");
    });

    it("matches an EOT-only envelope (test-only shape)", () => {
        // A real stream never sees "nothing but EOT": a committed txn always emits at
        // least one statement event before its EOT, so by EOT time the identity is
        // already bound. This shape exists only as a unit-level edge — note its
        // _txnId stays unbound, so the stream-level dedup would never register it.
        const eot = {operationType: "endOfTransaction", lsid: "L", txnNumber: NumberLong(1)};
        const matcher = new TxnChangeEventMatcher(new CommittedTxn([eot]), {
            eotMode: EotMode.kRequired,
        });
        assert.eq(matcher.getTotalCount(), 1, "The EOT is the whole envelope");
        assert(matcher.matches(eot, false), "The EOT matches immediately (the pool is empty)");
        assert(matcher.isDone(), "Done after the EOT");
        assert.eq(matcher.getMatchedCount(), 1, "Only the EOT matched");
        assert.eq(matcher.getTxnId(), null, "No statement event to bind the identity from");
    });

    it("position-locks the txn matcher once it consumed an event (skippable)", () => {
        const ev = (id) => ({
            operationType: "insert",
            ns: {db: "d", coll: "c"},
            fullDocument: {_id: id},
            lsid: "L",
            txnNumber: NumberLong(7),
        });
        const eot = {operationType: "endOfTransaction", lsid: "L", txnNumber: NumberLong(7)};
        const matcher = new TxnChangeEventMatcher(new CommittedTxn([ev("a"), ev("b"), eot]), {
            eotMode: EotMode.kRequired,
        });
        assert(matcher.skippable(), "A txn matcher with no consumed events may be skipped past");
        assert(matcher.matches(ev("a"), false), "First statement matches");
        // Partially consumed: the stream must never skip over it (matchesOrSkip).
        assert(!matcher.skippable(), "Partially consumed: position-locked");
    });

    it("closes a consumed ChangeEventMatcher", () => {
        const ev = {operationType: "insert", ns: {db: "d", coll: "c"}, fullDocument: {_id: "a"}};
        const matcher = new ChangeEventMatcher(ev);
        assert(matcher.matches(ev, false), "Event matches");
        assert(matcher.isDone(), "Done after the match");
        assert(!matcher.matches(ev, false), "A consumed matcher is closed");
        assert.eq(matcher.getMatchedCount(), 1, "Count frozen after done");
    });
});
