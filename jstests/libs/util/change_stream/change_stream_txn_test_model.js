/**
 * Test model for the transaction-centric change stream FSM tests (End of Transaction
 * testing). Provides the action and state enums and the TransactionTestModel state
 * machine the FsmCommandGenerator walk covers.
 */

import {GraphModel} from "jstests/libs/util/change_stream/change_stream_fsm_command_generator.js";

/**
 * Action IDs for the transaction-centric FSM test model (End of Transaction testing).
 * A parallel enum to `ShardingAction` (sharding model): each model's all-edges coverage check
 * iterates ONLY its own enum, so the models stay isolated.
 */
class TxnAction {
    static INSERT_AUTOCOMMIT = 0;
    static BEGIN_TXN_SHARDED_COLL = 1;
    static BEGIN_TXN_UNSHARDED_COLL = 2;
    static TXN_STMT_SAME_COLL_SAME_SHARD = 3;
    static TXN_STMT_SAME_COLL_NEW_SHARD = 4;
    static TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD = 5;
    static TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD = 6;
    static TXN_STMT_NEW_DB_SAME_SHARD = 7;
    static TXN_STMT_NEW_DB_NEW_SHARD = 8;
    static COMMIT_TXN = 9;
    static ABORT_TXN = 10;

    static getName(actionId) {
        switch (actionId) {
            case TxnAction.INSERT_AUTOCOMMIT:
                return "autocommit insert";
            case TxnAction.BEGIN_TXN_SHARDED_COLL:
                return "begin txn on sharded collection";
            case TxnAction.BEGIN_TXN_UNSHARDED_COLL:
                return "begin txn on unsharded collection";
            case TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD:
                return "txn insert, same collection, same shard";
            case TxnAction.TXN_STMT_SAME_COLL_NEW_SHARD:
                return "txn insert, same collection, new shard";
            case TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD:
                return "txn insert, new collection same db, same shard";
            case TxnAction.TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD:
                return "txn insert, new collection same db, new shard";
            case TxnAction.TXN_STMT_NEW_DB_SAME_SHARD:
                return "txn insert, new db, same shard";
            case TxnAction.TXN_STMT_NEW_DB_NEW_SHARD:
                return "txn insert, new db, new shard";
            case TxnAction.COMMIT_TXN:
                return "commit txn";
            case TxnAction.ABORT_TXN:
                return "abort txn";
            default:
                throw new Error(`Invalid txn action ID: ${actionId}`);
        }
    }
}

/**
 * Writer transaction-context states — one class per topology. The sharded class
 * encodes (dbCount, collCount, shardCount) with the one-coll row split by collection
 * kind (sharded vs unsharded) — the split that makes SAME_COLL_NEW_SHARD a statically
 * valid edge. The replica-set class has no shard dimension.
 */
class TxnStateReplicaSet {
    static TXN_IDLE = 0;
    static ONE_COLL = 1;
    static ONE_DB_MANY_COLL = 2;
    static MANY_DB_MANY_COLL = 3;

    static getName(stateId) {
        switch (stateId) {
            case TxnStateReplicaSet.TXN_IDLE:
                return "Idle";
            case TxnStateReplicaSet.ONE_COLL:
                return "OneColl";
            case TxnStateReplicaSet.ONE_DB_MANY_COLL:
                return "OneDbManyColl";
            case TxnStateReplicaSet.MANY_DB_MANY_COLL:
                return "ManyDbManyColl";
            default:
                throw new Error(`Invalid RS txn state ID: ${stateId}`);
        }
    }
}

class TxnStateShardedCluster {
    static TXN_IDLE = 0;
    static ONE_COLL_SHARDED_ONE_SHARD = 1;
    static ONE_COLL_SHARDED_MANY_SHARD = 2;
    static ONE_COLL_UNSHARDED_ONE_SHARD = 3;
    static ONE_DB_MANY_COLL_ONE_SHARD = 4;
    static ONE_DB_MANY_COLL_MANY_SHARD = 5;
    static MANY_DB_MANY_COLL_ONE_SHARD = 6;
    static MANY_DB_MANY_COLL_MANY_SHARD = 7;

    static getName(stateId) {
        switch (stateId) {
            case TxnStateShardedCluster.TXN_IDLE:
                return "Idle";
            case TxnStateShardedCluster.ONE_COLL_SHARDED_ONE_SHARD:
                return "OneColl::Sharded::OneShard";
            case TxnStateShardedCluster.ONE_COLL_SHARDED_MANY_SHARD:
                return "OneColl::Sharded::ManyShard";
            case TxnStateShardedCluster.ONE_COLL_UNSHARDED_ONE_SHARD:
                return "OneColl::Unsharded::OneShard";
            case TxnStateShardedCluster.ONE_DB_MANY_COLL_ONE_SHARD:
                return "OneDbManyColl::OneShard";
            case TxnStateShardedCluster.ONE_DB_MANY_COLL_MANY_SHARD:
                return "OneDbManyColl::ManyShard";
            case TxnStateShardedCluster.MANY_DB_MANY_COLL_ONE_SHARD:
                return "ManyDbManyColl::OneShard";
            case TxnStateShardedCluster.MANY_DB_MANY_COLL_MANY_SHARD:
                return "ManyDbManyColl::ManyShard";
            default:
                throw new Error(`Invalid sharded txn state ID: ${stateId}`);
        }
    }
}

/**
 * Finite state machine for End-of-Transaction testing: the writer's transaction
 * context. Transitions are defined by their (scopeDelta, participantDelta) effect; the
 * generator resolves each delta to a concrete collection via the fixture. The model is
 * just a graph — states, edges, start state; everything else (participating shards,
 * touched collections, accumulated commands) is the generator's walk context.
 */
class TxnModelReplicaSet extends GraphModel {
    constructor() {
        super();
        this._buildGraph();
    }

    getStartState() {
        return TxnStateReplicaSet.TXN_IDLE;
    }

    _buildGraph() {
        const S = TxnStateReplicaSet;
        this._addEdge(S.TXN_IDLE, TxnAction.INSERT_AUTOCOMMIT, S.TXN_IDLE);
        this._addEdge(S.TXN_IDLE, TxnAction.BEGIN_TXN_UNSHARDED_COLL, S.ONE_COLL);
        this._addEdge(S.ONE_COLL, TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD, S.ONE_COLL);
        this._addEdge(
            S.ONE_COLL,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD,
            S.ONE_DB_MANY_COLL,
        );
        this._addEdge(S.ONE_COLL, TxnAction.TXN_STMT_NEW_DB_SAME_SHARD, S.MANY_DB_MANY_COLL);
        this._addEdge(
            S.ONE_DB_MANY_COLL,
            TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD,
            S.ONE_DB_MANY_COLL,
        );
        this._addEdge(
            S.ONE_DB_MANY_COLL,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD,
            S.ONE_DB_MANY_COLL,
        );
        this._addEdge(
            S.ONE_DB_MANY_COLL,
            TxnAction.TXN_STMT_NEW_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL,
        );
        this._addEdge(
            S.MANY_DB_MANY_COLL,
            TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD,
            S.MANY_DB_MANY_COLL,
        );
        this._addEdge(
            S.MANY_DB_MANY_COLL,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL,
        );
        this._addEdge(
            S.MANY_DB_MANY_COLL,
            TxnAction.TXN_STMT_NEW_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL,
        );
        for (const state of [S.ONE_COLL, S.ONE_DB_MANY_COLL, S.MANY_DB_MANY_COLL]) {
            this._addEdge(state, TxnAction.COMMIT_TXN, S.TXN_IDLE);
            this._addEdge(state, TxnAction.ABORT_TXN, S.TXN_IDLE);
        }
    }
}

class TxnModelShardedCluster extends GraphModel {
    constructor() {
        super();
        this._buildGraph();
    }

    getStartState() {
        return TxnStateShardedCluster.TXN_IDLE;
    }

    _buildGraph() {
        const S = TxnStateShardedCluster;

        // IDLE: autocommit self-loop + both begin edges.
        this._addEdge(S.TXN_IDLE, TxnAction.INSERT_AUTOCOMMIT, S.TXN_IDLE);
        this._addEdge(S.TXN_IDLE, TxnAction.BEGIN_TXN_SHARDED_COLL, S.ONE_COLL_SHARDED_ONE_SHARD);
        this._addEdge(
            S.TXN_IDLE,
            TxnAction.BEGIN_TXN_UNSHARDED_COLL,
            S.ONE_COLL_UNSHARDED_ONE_SHARD,
        );

        // ONE_COLL_SHARDED_ONE_SHARD.
        this._addEdge(
            S.ONE_COLL_SHARDED_ONE_SHARD,
            TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD,
            S.ONE_COLL_SHARDED_ONE_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_SHARDED_ONE_SHARD,
            TxnAction.TXN_STMT_SAME_COLL_NEW_SHARD,
            S.ONE_COLL_SHARDED_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_SHARDED_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD,
            S.ONE_DB_MANY_COLL_ONE_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_SHARDED_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD,
            S.ONE_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_SHARDED_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL_ONE_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_SHARDED_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_DB_NEW_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );

        // ONE_COLL_SHARDED_MANY_SHARD (further NEW_SHARD statements are self-loops).
        this._addEdge(
            S.ONE_COLL_SHARDED_MANY_SHARD,
            TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD,
            S.ONE_COLL_SHARDED_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_SHARDED_MANY_SHARD,
            TxnAction.TXN_STMT_SAME_COLL_NEW_SHARD,
            S.ONE_COLL_SHARDED_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_SHARDED_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD,
            S.ONE_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_SHARDED_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD,
            S.ONE_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_SHARDED_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_SHARDED_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_DB_NEW_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );

        // ONE_COLL_UNSHARDED_ONE_SHARD — no MANY_SHARD state exists (an unsharded
        // collection cannot span shards), so the participant upgrade requires a new
        // collection.
        this._addEdge(
            S.ONE_COLL_UNSHARDED_ONE_SHARD,
            TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD,
            S.ONE_COLL_UNSHARDED_ONE_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_UNSHARDED_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD,
            S.ONE_DB_MANY_COLL_ONE_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_UNSHARDED_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD,
            S.ONE_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_UNSHARDED_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL_ONE_SHARD,
        );
        this._addEdge(
            S.ONE_COLL_UNSHARDED_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_DB_NEW_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );

        // ONE_DB_MANY_COLL_*.
        this._addEdge(
            S.ONE_DB_MANY_COLL_ONE_SHARD,
            TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD,
            S.ONE_DB_MANY_COLL_ONE_SHARD,
        );
        this._addEdge(
            S.ONE_DB_MANY_COLL_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD,
            S.ONE_DB_MANY_COLL_ONE_SHARD,
        );
        this._addEdge(
            S.ONE_DB_MANY_COLL_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD,
            S.ONE_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_DB_MANY_COLL_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL_ONE_SHARD,
        );
        this._addEdge(
            S.ONE_DB_MANY_COLL_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_DB_NEW_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );

        this._addEdge(
            S.ONE_DB_MANY_COLL_MANY_SHARD,
            TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD,
            S.ONE_DB_MANY_COLL_MANY_SHARD,
        );
        // No SAME_COLL_NEW_SHARD from either *_MANY_COLL_MANY_SHARD state: in a
        // many-coll state the last-touched collection may be an unsharded one, which
        // cannot reach a new shard — the edge is only statically valid from
        // ONE_COLL_SHARDED_* states.
        this._addEdge(
            S.ONE_DB_MANY_COLL_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD,
            S.ONE_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_DB_MANY_COLL_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD,
            S.ONE_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_DB_MANY_COLL_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.ONE_DB_MANY_COLL_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_DB_NEW_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );

        // MANY_DB_MANY_COLL_*.
        this._addEdge(
            S.MANY_DB_MANY_COLL_ONE_SHARD,
            TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD,
            S.MANY_DB_MANY_COLL_ONE_SHARD,
        );
        this._addEdge(
            S.MANY_DB_MANY_COLL_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL_ONE_SHARD,
        );
        this._addEdge(
            S.MANY_DB_MANY_COLL_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL_ONE_SHARD,
        );
        this._addEdge(
            S.MANY_DB_MANY_COLL_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.MANY_DB_MANY_COLL_ONE_SHARD,
            TxnAction.TXN_STMT_NEW_DB_NEW_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );

        this._addEdge(
            S.MANY_DB_MANY_COLL_MANY_SHARD,
            TxnAction.TXN_STMT_SAME_COLL_SAME_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.MANY_DB_MANY_COLL_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.MANY_DB_MANY_COLL_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_COLL_SAME_DB_NEW_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.MANY_DB_MANY_COLL_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_DB_SAME_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );
        this._addEdge(
            S.MANY_DB_MANY_COLL_MANY_SHARD,
            TxnAction.TXN_STMT_NEW_DB_NEW_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        );

        // COMMIT/ABORT from every in-txn state.
        for (const state of [
            S.ONE_COLL_SHARDED_ONE_SHARD,
            S.ONE_COLL_SHARDED_MANY_SHARD,
            S.ONE_COLL_UNSHARDED_ONE_SHARD,
            S.ONE_DB_MANY_COLL_ONE_SHARD,
            S.ONE_DB_MANY_COLL_MANY_SHARD,
            S.MANY_DB_MANY_COLL_ONE_SHARD,
            S.MANY_DB_MANY_COLL_MANY_SHARD,
        ]) {
            this._addEdge(state, TxnAction.COMMIT_TXN, S.TXN_IDLE);
            this._addEdge(state, TxnAction.ABORT_TXN, S.TXN_IDLE);
        }
    }
}

export {
    TxnAction,
    TxnModelReplicaSet,
    TxnModelShardedCluster,
    TxnStateReplicaSet,
    TxnStateShardedCluster,
};
