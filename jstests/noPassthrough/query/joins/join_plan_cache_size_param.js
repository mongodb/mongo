/**
 * Verifies the 'internalQueryJoinPlanCacheSize' startup parameter that bounds the join plan cache:
 * it accepts both byte and percentage sizes at startup, rejects malformed values (failing startup),
 * and is not settable at runtime.
 */

import {describe, it} from "jstests/libs/mochalite.js";

const kParam = "internalQueryJoinPlanCacheSize";

// Starts a mongod with the parameter set to 'value', asserts getParameter echoes it back, then stops
// the server.
function assertStartupValueRoundtrips(value) {
    const conn = MongoRunner.runMongod({setParameter: {[kParam]: value}});
    try {
        const res = assert.commandWorked(conn.adminCommand({getParameter: 1, [kParam]: 1}));
        assert.eq(res[kParam], value, "getParameter did not echo the startup value");
    } finally {
        MongoRunner.stopMongod(conn);
    }
}

describe("internalQueryJoinPlanCacheSize", function () {
    it("accepts a byte size at startup", function () {
        assertStartupValueRoundtrips("1MB");
    });

    it("accepts a percentage at startup", function () {
        assertStartupValueRoundtrips("2%");
    });

    it("accepts larger size than cap", function () {
        // Internally this parameter is capped to 500GB and 25%, so we can test a larger value to
        // ensure it is accepted but we don't have a way to expose the capped size.
        assertStartupValueRoundtrips("600GB");
        assertStartupValueRoundtrips("50%");
    });

    it("rejects a malformed size at startup", function () {
        // A value the memory-size validator cannot parse must prevent mongod from starting.
        assert.throws(() => MongoRunner.runMongod({setParameter: {[kParam]: "notasize"}}));
    });

    it("is not settable at runtime", function () {
        const conn = MongoRunner.runMongod({});
        try {
            assert.commandFailedWithCode(
                conn.adminCommand({setParameter: 1, [kParam]: "10MB"}),
                ErrorCodes.IllegalOperation,
            );
        } finally {
            MongoRunner.stopMongod(conn);
        }
    });
});
