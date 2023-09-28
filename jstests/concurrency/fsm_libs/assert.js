/**
 * Helpers for controlling under which situations an assert is actually executed.
 * This allows us to define workloads that will have only valid assertions,
 * regardless of how any particular workload gets run with any others.
 *
 * There are 3 different assert levels:
 *   ALWAYS   = these assertions are always executed
 *   OWN_COLL = these assertions are executed when workloads are run on separate collections
 *   OWN_DB   = these assertions are executed when workloads are run on separate databases
 */

export var AssertLevel = (function() {
    function AssertLevel(level) {
        this.level = level;

        // Returns < 0 if this < other
        //         = 0 if this == other
        //         > 0 if this > other
        this.compareTo = function(other) {
            return this.level - other.level;
        };
    }

    function isAssertLevel(obj) {
        return obj instanceof AssertLevel;
    }

    return {
        ALWAYS: new AssertLevel(0),
        OWN_COLL: new AssertLevel(1),
        OWN_DB: new AssertLevel(2),
        isAssertLevel: isAssertLevel
    };
})();

let globalAssertLevel = AssertLevel.OWN_DB;

export function getGlobalAssertLevel() {
    return globalAssertLevel;
}

export function setGlobalAssertLevel(level) {
    globalAssertLevel = level;
}

export var assertWithLevel = function(level) {
    assert(AssertLevel.isAssertLevel(level), 'expected AssertLevel as first argument');

    function quietlyDoAssert(msg, obj) {
        // eval if msg is a function
        if (typeof msg === 'function') {
            msg = msg();
        }

        var ex;
        if (obj) {
            ex = _getErrorWithCode(obj, msg);
        } else {
            ex = new Error(msg);
        }

        throw ex;
    }

    function wrapAssertFn(fn, args) {
        let res;
        var doassertSaved = doassert;
        try {
            doassert = quietlyDoAssert;
            res = fn.apply(assert, args);  // functions typically get called on 'assert'
        } finally {
            doassert = doassertSaved;
        }

        return res;
    }

    var assertWithLevel = function() {
        // Only execute assertion if level for which it was defined is
        // a subset of the global assertion level
        if (level.compareTo(globalAssertLevel) > 0) {
            return;
        }

        if (arguments.length === 1 && typeof arguments[0] === 'function') {
            // Assert against the value returned by the function
            arguments[0] = arguments[0]();

            // If a function does not explictly return a value,
            // then have it implicitly return true
            if (typeof arguments[0] === 'undefined') {
                arguments[0] = true;
            }
        }

        wrapAssertFn(assert, arguments);
    };

    Object.keys(assert).forEach(function(fn) {
        if (typeof assert[fn] !== 'function') {
            return;
        }

        assertWithLevel[fn] = function() {
            // Only execute assertion if level for which it was defined is
            // a subset of the global assertion level
            if (level.compareTo(globalAssertLevel) > 0) {
                return;
            }

            return wrapAssertFn(assert[fn], arguments);
        };
    });

    return assertWithLevel;
};

export var assertAlways = assertWithLevel(AssertLevel.ALWAYS);
export var assertWhenOwnColl = assertWithLevel(AssertLevel.OWN_COLL);
export var assertWhenOwnDB = assertWithLevel(AssertLevel.OWN_DB);

/**
 * Any query that uses getMore is vulnerable to failures that result from the query being killed by
 * a stepdown or similar process before its cursor is exhausted. This includes any aggregation on a
 * sharded cluster, because the mongos always uses getMore to get results from shards.
 *
 * Workloads that issue queries requiring multiple batches or sharded execution plans should detect
 * these errors and ensure that they do not get reported as test failures.
 */
export var interruptedQueryErrors = [
    ErrorCodes.CursorNotFound,
    ErrorCodes.CursorKilled,
    ErrorCodes.Interrupted,
    ErrorCodes.QueryPlanKilled
];