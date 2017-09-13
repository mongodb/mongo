'use strict';

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

var AssertLevel = (function() {

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

if (typeof globalAssertLevel === 'undefined') {
    var globalAssertLevel = AssertLevel.OWN_DB;
}

var assertWithLevel = function(level) {
    assert(AssertLevel.isAssertLevel(level), 'expected AssertLevel as first argument');

    function quietlyDoAssert(msg) {
        // eval if msg is a function
        if (typeof msg === 'function') {
            msg = msg();
        }

        throw new Error(msg);
    }

    function wrapAssertFn(fn, args) {
        var doassertSaved = doassert;
        try {
            doassert = quietlyDoAssert;
            fn.apply(assert, args);  // functions typically get called on 'assert'
        } finally {
            doassert = doassertSaved;
        }
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

            wrapAssertFn(assert[fn], arguments);
        };
    });

    return assertWithLevel;
};

var assertAlways = assertWithLevel(AssertLevel.ALWAYS);
var assertWhenOwnColl = assertWithLevel(AssertLevel.OWN_COLL);
var assertWhenOwnDB = assertWithLevel(AssertLevel.OWN_DB);
