/**
 * Enables causal consistency on the connections.
 */
(function() {
    "use strict";

    db.getMongo().setCausalConsistency();
    db.getMongo().setReadPref("secondary");

    var originalStartParallelShell = startParallelShell;
    startParallelShell = function(jsCode, port, noConnect) {
        var newCode;
        var overridesFile = "jstests/libs/override_methods/enable_causal_consistency.js";
        if (typeof(jsCode) === "function") {
            // Load the override file and immediately invoke the supplied function.
            newCode = `load("${overridesFile}"); (${jsCode})();`;
        } else {
            newCode = `load("${overridesFile}"); ${jsCode};`;
        }

        return originalStartParallelShell(newCode, port, noConnect);
    };
})();
