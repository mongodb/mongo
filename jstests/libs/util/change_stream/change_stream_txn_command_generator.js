/**
 * Command generator for the transaction-centric FSM test model (End of Transaction
 * testing). Subclasses the model-agnostic FsmCommandGenerator walk and adds:
 * 1. Cross-transition accumulation: a txn's command series spans walk transitions
 *    (BEGIN…COMMIT are separate edges), so the generator accumulates the open txn's
 *    TxnOpCommands in the walk context (context.inFlightCommands — commands as context,
 *    not as a shared output array) and passes the list to
 *    CommitTxnCommand(commandsInTxn) at the commit edge. Hooks RETURN the transition's
 *    commands (_commandsForAction → Command[]); the base owns the flat list.
 * 2. Walk-end closure: _finalCommands appends COMMIT_TXN when the walk finishes
 *    mid-txn.
 *
 * Collection first, shard second: the generator picks the collection an op targets,
 * and the shard follows from it — an unsharded collection lives on exactly one shard,
 * so its placement DECIDES the shard; only the sharded collection leaves a choice, and
 * that choice is which shard-key range to write.
 */
import {FsmCommandGenerator} from "jstests/libs/util/change_stream/change_stream_fsm_command_generator.js";
import {TxnAction} from "jstests/libs/util/change_stream/change_stream_txn_test_model.js";
import {
    BeginTxnCommand,
    CommitTxnCommand,
    AbortTxnCommand,
    InsertDocCommand,
    TxnOpCommand,
} from "jstests/libs/util/change_stream/change_stream_commands.js";

// Per-shard shard-key range width. Each shard owns [i*W, (i+1)*W); the walk's insert
// count must stay far below W. Also drives the Phase B presplit boundaries.
const kShardRangeWidth = 100000;

// The MANY_SHARD txn states are defined as spanning at least this many shards.
const kManyShardMinShards = 2;

/**
 * One fixture collection with everything the walk needs:
 * - ns: {db, coll} — matches the change-event ns shape.
 * - placementShards: where an op on this collection can land. An unsharded
 *   collection lives on exactly ONE shard (its placement DECIDES the shard); a
 *   sharded collection SPANS the listed shards — each op's shard derives from the
 *   written document's shard-key range.
 * - shardedConfig: null for an unsharded collection; for a sharded one, the
 *   shardKey (the event's documentKey gains those fields) and the presplit
 *   rangeWidth (an op forces its placement by writing a shard-key value inside the
 *   chosen range: shardIdx * rangeWidth + randInt(rangeWidth)).
 */
class TxnCommandGeneratorParams {
    constructor({colls, eotGating = "kEnabled"}) {
        this.colls = colls;
        // The cluster's shard ids: the union of the colls' placementShards (first-seen
        // order = the presplit range order — only the sharded collection spans more
        // than one shard).
        this.shardIds = [];
        for (const coll of colls) {
            for (const shardId of coll.placementShards) {
                if (!this.shardIds.includes(shardId)) {
                    this.shardIds.push(shardId);
                }
            }
        }
        this.eotGating = eotGating; // variant-level gating: "kEnabled" | "kDisabled" | "kNondeterministic"
    }

    /** The distinct dbs of the fixture collections, inferred from colls. */
    getAllDbs() {
        return [...new Set(this.colls.map((c) => c.ns.db))];
    }
}

function nsKey(ns) {
    return `${ns.db}.${ns.coll}`;
}

class TxnCommandGenerator extends FsmCommandGenerator {
    _initialContextForState() {
        return {
            txnInProgress: false, // is a txn currently in flight
            // Concrete shard indexes the open txn targets. Irrelevant on a replica set:
            // the RS graph has no NEW_SHARD edges, so this set is never consulted.
            participatingShards: new Set(),
            touchedColls: new Set(), // "db.coll" strings touched by the open txn
            touchedDbs: new Set(), // dbs touched by the open txn
            inFlightCommands: [], // the in-flight txn's accumulated TxnOpCommands
        };
    }

    _isTouched(context, ns) {
        return context.touchedColls.has(nsKey(ns));
    }

    /**
     * Can an op on `coll` land on the shard this edge demands?
     * - A sharded collection spans every shard (on a replica set too — the single
     *   logical shard is simply the only range there): the shard follows from the
     *   shard-key RANGE we
     *   write, so same-shard is always satisfiable and new-shard is satisfiable while
     *   a non-participating range remains.
     * - An unsharded collection lives on exactly one shard: its placement DECIDES the
     *   shard, so it qualifies only if that shard is (not) already participating.
     */
    _satisfiesShardRequirement(params, context, coll, wantNewShard) {
        if (coll.shardedConfig !== null) {
            if (!wantNewShard) {
                return true;
            }
            // A fresh range is needed only while the edge UPGRADES the txn's span
            // (one participating shard); once the txn already spans many shards, the
            // MANY_SHARD state stays truthful with any range.
            if (context.participatingShards.size < kManyShardMinShards) {
                return context.participatingShards.size < params.shardIds.length;
            }
            return true;
        }
        const idx = this._shardIdxOf(params, coll.placementShards[0]);
        if (!wantNewShard) {
            return context.participatingShards.size === 0 || context.participatingShards.has(idx);
        }
        // Same MANY_SHARD-truthful rule: a NOT-yet-participating shard is required
        // only for the upgrade; afterwards any placement keeps the state truthful.
        if (context.participatingShards.size < kManyShardMinShards) {
            return !context.participatingShards.has(idx);
        }
        return true;
    }

    /**
     * The shard an op on `coll` lands on. For an unsharded collection the shard is
     * simply where it lives; only the sharded collection leaves a choice, and that
     * choice is which key range to write.
     */
    _resolveShardIdx(params, context, coll, wantNewShard) {
        if (coll.shardedConfig === null) {
            return this._shardIdxOf(params, coll.placementShards[0]);
        }
        // A sharded collection spans every presplit range: the op's RANGE INDEX is a
        // choice, and the written shard-key value encodes it (data = idx * rangeWidth
        // + rand) — which is why this machinery works in range indexes rather than
        // shard ids.
        const allRangeIdxs = params.shardIds.map((_, i) => i);
        let pool = wantNewShard
            ? allRangeIdxs.filter((i) => !context.participatingShards.has(i))
            : allRangeIdxs.filter((i) => context.participatingShards.has(i));
        if (
            wantNewShard &&
            pool.length === 0 &&
            context.participatingShards.size >= kManyShardMinShards
        ) {
            // No fresh range left but the txn already spans many shards: any range
            // keeps the MANY_SHARD state truthful.
            pool = allRangeIdxs;
        }
        // `pool` is empty only at BEGIN (nothing participates yet), where any range works.
        const usable = pool.length > 0 ? pool : allRangeIdxs;
        return usable[Random.randInt(usable.length)];
    }

    /**
     * An UNTOUCHED collection matching the requested shape, or null when none exists.
     * Null rather than an assert so call sites decide what an empty candidate set means.
     * `sharded` is tri-state: true = the sharded collection only, false = unsharded
     * only, null (default) = either. On a replica set nothing is sharded, so
     * `sharded: false` admits every fixture collection and `sharded: true` admits none
     * — which is consistent: the RS graph never emits BEGIN_TXN_SHARDED_COLL.
     */
    _pickColl(
        params,
        context,
        {inDb = null, wantNewShard = false, sharded = null, allowTouched = false} = {},
    ) {
        // inDb: constrain candidates to one database (null = any). sharded is tri-state:
        // true = sharded collections only, false = unsharded only, null = either.
        const pickShape = ({inDb, wantNewShard, sharded, touched}) => {
            const candidates = params.colls.filter((coll) => {
                if (!touched && this._isTouched(context, coll)) return false;
                if (inDb !== null && coll.ns.db !== inDb) return false;
                if (sharded !== null && (coll.shardedConfig !== null) !== sharded) return false;
                return this._satisfiesShardRequirement(params, context, coll, wantNewShard);
            });
            return candidates.length > 0 ? candidates[Random.randInt(candidates.length)] : null;
        };

        let coll = pickShape({inDb, wantNewShard, sharded, touched: false});
        if (coll === null && allowTouched) {
            coll = pickShape({inDb, wantNewShard, sharded, touched: true});
        }
        return coll;
    }

    /**
     * A document for an op on `coll`, targeting `shardIdx`. For a sharded collection
     * the shard-key field carries a value inside that shard's presplit range (the
     * value decides placement); for an unsharded one `data` is inert. Documents are
     * distinguished by their fresh _id, so shard-key values need not be unique.
     */
    _makeDoc(coll, shardIdx) {
        if (coll.shardedConfig === null) {
            return {_id: new ObjectId().str, data: Random.randInt(1000)};
        }
        const keyField = Object.keys(coll.shardedConfig.shardKey)[0];
        const w = coll.shardedConfig.rangeWidth;
        return {
            _id: new ObjectId().str,
            [keyField]: shardIdx * w + Random.randInt(w),
        };
    }

    /**
     * collectionCtx must carry the shard key for the SHARDED collection: a change
     * event's documentKey is {shardKeyFields..., _id}, and ChangeEventMatcher compares
     * documentKey as a whole value — an _id-only expectation would never match on a
     * sharded cluster. Unsharded targets keep an _id-only documentKey.
     */
    _collectionCtxFor(coll) {
        return coll.shardedConfig !== null
            ? {exists: true, shardKeySpec: coll.shardedConfig.shardKey}
            : {exists: true};
    }

    _appendTxnOp(params, context, coll, wantNewShard = false, out = []) {
        const shardIdx = this._resolveShardIdx(params, context, coll, wantNewShard);
        const op = new TxnOpCommand({
            inner: new InsertDocCommand({
                dbName: coll.ns.db,
                collName: coll.ns.coll,
                collectionCtx: this._collectionCtxFor(coll),
                documents: [this._makeDoc(coll, shardIdx)],
            }),
        });
        // The op goes FLAT into the walk (the Writer executes each statement on its
        // own) AND into inFlightCommands — the commit's prediction aggregates the same
        // command objects.
        out.push(op);
        context.inFlightCommands.push(op);
        context.touchedColls.add(nsKey(coll.ns));
        context.touchedDbs.add(coll.ns.db);
        context.participatingShards.add(shardIdx);
        return op;
    }

    _lastTouchedColl(context) {
        // The collection the txn's most recent statement touched. TxnOpCommand is a
        // pure wrapper (no ns fields of its own) — read through to the inner command.
        const last = context.inFlightCommands[context.inFlightCommands.length - 1];
        assert(last instanceof TxnOpCommand, "The open txn's last command must be a statement");
        return {db: last.inner.dbName, coll: last.inner.collName};
    }

    /**
     * The presplit range index of a SHARD ID: the i-th cluster shard guards the
     * shard-key range [i * rangeWidth, (i + 1) * rangeWidth).
     */
    _shardIdxOf(params, shardId) {
        const idx = params.shardIds.indexOf(shardId);
        assert.gte(idx, 0, `Unknown shardId: ${shardId}`);
        return idx;
    }

    /**
     * The FULL collInfo for a namespace: _lastTouchedColl can only carry
     * {dbName, collName} (the inner command has no placement fields), so re-resolve
     * against the fixture to recover the placement data. STATIC fixture data — it
     * lives in params, not in the walk context (the context is per-walk mutable state).
     */
    _getCollInfo(params, ns) {
        const coll = params.colls.find((c) => nsKey(c.ns) === nsKey(ns));
        assert(coll !== undefined, `Unknown collection ${ns.dbName}.${ns.collName}`);
        return coll;
    }

    // Every hook RETURNS the transition's commands (the base owns the flat list);
    // txn accumulation (inFlightCommands) lives in the walk context, not in the base.
    _commandsForAction(action, params, context) {
        const out = [];
        switch (action) {
            case TxnAction.INSERT_AUTOCOMMIT: {
                // Outside a txn nothing is touched or participating, so every fixture
                // collection is a candidate.
                const coll = this._pickColl(params, context);
                assert(coll, "Fixture has no collection to insert into");
                out.push(
                    new InsertDocCommand({
                        dbName: coll.ns.db,
                        collName: coll.ns.coll,
                        collectionCtx: this._collectionCtxFor(coll),
                        documents: [
                            this._makeDoc(
                                coll,
                                this._resolveShardIdx(params, context, coll, false),
                            ),
                        ],
                    }),
                );
                break;
            }
            case TxnAction.BEGIN_TXN_SHARDED_COLL:
            case TxnAction.BEGIN_TXN_UNSHARDED_COLL: {
                assert(!context.txnInProgress, "Walk emitted BEGIN inside an in-flight txn");
                context.txnInProgress = true;
                out.push(new BeginTxnCommand());
                // Pick the COLLECTION first; the shard follows from it (and, for the
                // sharded collection, from the key range the op writes).
                const wantSharded = action === TxnAction.BEGIN_TXN_SHARDED_COLL;
                const coll = this._pickColl(params, context, {sharded: wantSharded});
                assert(
                    coll !== null,
                    `Fixture has no ${wantSharded ? "sharded" : "unsharded"} collection to begin a txn on`,
                );
                // ONE statement: the txn lands in a ONE_COLL state, and the
                // TXN_STMT_SAME_COLL_SAME_SHARD self-loop grows it from there.
                this._appendTxnOp(params, context, coll, /*wantNewShard=*/ false, out);
                break;
            }
            case TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD: {
                assert(context.txnInProgress, "TXN_STMT outside an in-flight txn");
                const lastCollNs = this._lastTouchedColl(context);
                this._appendTxnOp(
                    params,
                    context,
                    this._getCollInfo(params, lastCollNs),
                    false,
                    out,
                );
                break;
            }
            case TxnAction.TXN_STMT_SAME_COLL_NEW_SHARD: {
                assert(context.txnInProgress, "TXN_STMT outside an in-flight txn");
                const lastCollNs = this._lastTouchedColl(context);
                const coll = this._getCollInfo(params, lastCollNs);
                // Only the sharded collection can reach a new shard without changing
                // collection; the graph offers this edge from ONE_COLL_SHARDED_* only.
                assert(
                    coll.shardedConfig !== null,
                    "SAME_COLL_NEW_SHARD on an unsharded collection",
                );
                this._appendTxnOp(params, context, coll, /*wantNewShard=*/ true, out);
                break;
            }
            case TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD:
            case TxnAction.TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD:
            case TxnAction.TXN_STMT_NEW_DB_SAME_SHARD:
            case TxnAction.TXN_STMT_NEW_DB_NEW_SHARD: {
                assert(context.txnInProgress, "TXN_STMT outside an in-flight txn");
                const wantNewShard =
                    action === TxnAction.TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD ||
                    action === TxnAction.TXN_STMT_NEW_DB_NEW_SHARD;
                const sameDb =
                    action === TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD ||
                    action === TxnAction.TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD;
                const lastDb = this._lastTouchedColl(context).db;
                const inDb = sameDb ? lastDb : params.getAllDbs().find((db) => db !== lastDb);
                // An UNTOUCHED collection is required only when the edge actually
                // upgrades the scope (txn still ONE_COLL / single-db); from a
                // many-coll / many-db state a re-touch keeps the state truthful and
                // the fixture pool cannot be exhausted.
                const canRetouch = sameDb
                    ? context.touchedColls.size >= 2
                    : context.touchedDbs.size >= 2;
                const coll = this._pickColl(params, context, {
                    inDb,
                    wantNewShard,
                    allowTouched: canRetouch,
                });
                assert(
                    coll !== null,
                    `No collection in ${inDb} satisfying wantNewShard=${wantNewShard}`,
                );
                this._appendTxnOp(params, context, coll, wantNewShard, out);
                break;
            }
            case TxnAction.COMMIT_TXN: {
                assert(context.txnInProgress, "COMMIT_TXN outside an in-flight txn");
                out.push(
                    new CommitTxnCommand({
                        // The command keeps a reference to the accumulated array; the
                        // post-hook's reset installs a FRESH array (never mutates this
                        // one), so the committed txn's statements survive.
                        commandsInTxn: context.inFlightCommands,
                        includeEot: params.eotGating === "kEnabled",
                    }),
                );
                break;
            }
            case TxnAction.ABORT_TXN: {
                assert(context.txnInProgress, "ABORT_TXN outside an in-flight txn");
                out.push(new AbortTxnCommand());
                break;
            }
            default:
                throw new Error(`Unhandled txn action: ${action}`);
        }
        return out;
    }

    _updateContextForAction(action, context) {
        // The txn-accumulation state describes the OPEN txn only; leaving it set after
        // the txn closes leaks into TXN_IDLE and starves the autocommit picks. Reset
        // IN PLACE after the two CLOSING actions (a fresh inFlightCommands array — the
        // committed command keeps a reference to the old one). BEGIN needs no reset
        // (the state is already fresh after a close), and autocommit ops touch none of
        // it.
        if (action === TxnAction.COMMIT_TXN || action === TxnAction.ABORT_TXN) {
            Object.assign(context, this._initialContextForState());
        }
    }

    _finalCommands(params, context) {
        // Walk-end closure: the all-edges traversal CAN end mid-txn — the last
        // unvisited edge may be a BEGIN or a TXN_STMT, leaving the walk inside an open
        // txn. The closure commits so the sequence always ends at TXN_IDLE (committed);
        // the generator test asserts exactly that by tracking the txn state end-to-end.
        // Coverage itself needs no bookkeeping here: the walk visits EVERY edge, so
        // every shape the graph can express (one-shard and many-shard commits,
        // multi-statement txns, each scope upgrade) is generated by construction, and
        // the generator test asserts all-edges coverage directly.
        if (!context.txnInProgress) {
            return [];
        }
        const closing = [
            new CommitTxnCommand({
                commandsInTxn: context.inFlightCommands,
                includeEot: params.eotGating === "kEnabled",
            }),
        ];
        Object.assign(context, this._initialContextForState());
        return closing;
    }
}

export {TxnCommandGenerator, TxnCommandGeneratorParams, kShardRangeWidth};
