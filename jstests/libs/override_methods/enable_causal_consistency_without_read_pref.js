/**
 * Enables causal consistency on the connections without setting the read preference to secondary.
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");

    db.getMongo().setCausalConsistency();

    OverrideHelpers.prependOverrideInParallelShell(
        "jstests/libs/override_methods/enable_causal_consistency_without_read_pref.js");
})();
