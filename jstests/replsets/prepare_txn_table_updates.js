/**
 * Test different prepare-transaction oplog formats and verify that the config.transactions table
 * entries match between the primary and secondary.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {checkPrepareTxnTableUpdate} from "jstests/replsets/libs/prepare_txn_table_updates_helper.js";

function doTest(commitOrAbort) {
    const replSet = new ReplSetTest({
        nodes: [{}, {rsConfig: {priority: 0, votes: 0}}],
    });
    replSet.startSet(PrepareHelpers.replSetStartSetOptions);
    replSet.initiate();

    const primary = replSet.getPrimary();
    const secondary = replSet.getSecondary();
    checkPrepareTxnTableUpdate(primary, secondary, commitOrAbort);

    // Verify all config.transactions entries after all transactions have completed.
    replSet.stopSet();
}

doTest("commit");
doTest("abort");
