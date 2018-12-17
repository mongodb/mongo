'use strict';

/**
 * Runs update, findAndModify, delete, find, and getMore in a transaction with all threads using the
 * same session.
 *
 * @tags: [uses_transactions, state_functions_share_transaction, assumes_snapshot_transactions]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/multi_statement_transaction_all_commands.js');  // for
                                                                                        // $config

var $config = extendWorkload($config, function($config, $super) {

    $config.setup = function(db, collName, cluster) {
        $super.setup.apply(this, arguments);
        this.lsid = tojson({id: UUID()});
    };

    $config.states.init = function init(db, collName) {
        const lsid = eval(`(${this.lsid})`);
        this.session = db.getMongo().startSession({causalConsistency: true});
        // Force the session to use `lsid` for its session id. This way all threads will use
        // the same session.
        const oldId = this.session._serverSession.handle.getId();
        print("Overriding sessionID " + tojson(oldId) + " with " + tojson(lsid) + " for test.");
        this.session._serverSession.handle.getId = () => lsid;

        this.sessionDb = this.session.getDatabase(db.getName());
        this.iteration = 1;

        this.session.startTransaction_forTesting({readConcern: {level: 'snapshot'}});
        this.txnNumber = 0;
    };

    return $config;
});
