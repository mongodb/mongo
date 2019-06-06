/**
 * Tests that prepared transactions can safely be committed after a failover due to reconfig. We
 * issue the reconfig command against the secondary in this test, so that the primary can learn of
 * the new config via a heartbeat from that secondary.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
    "use strict";
    load("jstests/replsets/libs/prepare_failover_due_to_reconfig.js");

    let testName = "prepare_survives_reconfig_via_heartbeat_failover";

    testPrepareFailoverDueToReconfig(testName, /* reconfigOnPrimary */ false);
})();
