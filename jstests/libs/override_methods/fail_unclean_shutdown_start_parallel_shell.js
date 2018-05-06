/**
 * This override cowardly fails if a parallel shell is started because the shell will exit after an
 * unclean shutdown and won't be restarted when the node is restarted.
 */
(function() {
    "use strict";

    startParallelShell = function(jsCode, port, noConnect) {
        throw new Error("Cowardly fail if startParallelShell is run with a mongod that had" +
                        " an unclean shutdown.");
    };
})();
