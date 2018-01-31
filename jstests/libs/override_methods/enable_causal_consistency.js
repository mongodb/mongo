/**
 * Enables causal consistency on the connections.
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");

    db.getMongo().setCausalConsistency();
    db.getMongo().setReadPref("secondary");

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/enable_causal_consistency.js");
})();
