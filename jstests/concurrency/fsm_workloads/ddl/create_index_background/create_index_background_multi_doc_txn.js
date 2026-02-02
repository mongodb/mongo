/**
 * Executes the create_index_background.js workload, but with all writes as multidocument transactions.
 *
 * This workload implicitly assumes that its tid ranges are [0, $config.threadCount). This
 * isn't guaranteed to be true when they are run in parallel with other workloads. Therefore
 * it can't be run in concurrency simultaneous suites.
 * @tags: [
 *   assumes_balancer_off,
 *   creates_background_indexes,
 *   requires_getmore,
 *   incompatible_with_concurrency_simultaneous,
 *   uses_transactions,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/ddl/create_index_background/create_index_background.js";
import {withTxnAndAutoRetry} from "jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments); // base seeding + bg index coordination

        this.session = db.getMongo().startSession();
        this.sessionDB = this.session.getDatabase(db.getName());
    };

    function runInTxn(self, fn, {retryOnKilledSession = false} = {}) {
        withTxnAndAutoRetry(self.session, () => fn(), {retryOnKilledSession});
    }

    $config.states.createDocs = function (db, collName) {
        runInTxn(this, () => {
            $super.states.createDocs.call(this, this.sessionDB, collName);
        });
    };

    $config.states.updateDocs = function (db, collName) {
        runInTxn(this, () => {
            $super.states.updateDocs.call(this, this.sessionDB, collName);
        });
    };

    $config.states.deleteDocs = function (db, collName) {
        runInTxn(this, () => {
            $super.states.deleteDocs.call(this, this.sessionDB, collName);
        });
    };

    return $config;
});
