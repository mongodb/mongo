/**
 * Tests that prepared transactions can safely be committed after a failover due to reconfig. We
 * issue the reconfig command directly against the primary in this test.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";
load("jstests/replsets/libs/prepare_failover_due_to_reconfig.js");

let testName = "prepare_survives_primary_reconfig_failover";

testPrepareFailoverDueToReconfig(testName, /* reconfigOnPrimary */ true);
})();
