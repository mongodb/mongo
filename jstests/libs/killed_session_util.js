/**
 * Utilities for testing when sessions are killed.
 */
var KilledSessionUtil = (function() {
    // Returns if the code is one that could come from a session being killed.
    function isKilledSessionCode(code) {
        return code === ErrorCodes.Interrupted || code === ErrorCodes.CursorKilled ||
            code === ErrorCodes.CursorNotFound || code === ErrorCodes.QueryPlanKilled;
    }

    function hasKilledSessionError(errOrRes) {
        return isKilledSessionCode(errOrRes.code) ||
            (Array.isArray(errOrRes.writeErrors) &&
             errOrRes.writeErrors.every(writeError => isKilledSessionCode(writeError.code)));
    }

    function hasKilledSessionWCError(res) {
        return res.writeConcernError && isKilledSessionCode(res.writeConcernError.code);
    }

    return {
        isKilledSessionCode,
        hasKilledSessionError,
        hasKilledSessionWCError,
    };
})();
