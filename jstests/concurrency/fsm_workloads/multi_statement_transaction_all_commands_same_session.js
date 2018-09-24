'use strict';

/**
 * Runs update, findAndModify, delete, find, and getMore in a transaction with all threads using the
 * same session.
 *
 * @tags: [uses_transactions]
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
        this.session._serverSession.handle.getId = () => lsid;

        this.txnNumber = -1;
        this.sessionDb = this.session.getDatabase(db.getName());
        this.iteration = 1;
    };

    return $config;
});
