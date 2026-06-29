/**
 * Tests that an extension can uassert or error at parse time or during optimization.
 *
 * Note that this test does not trigger any tasserts to prevent test suite failures.
 * That path will be covered in unit tests which can gracefully handle death tests.
 *
 * @tags: [featureFlagExtensionsAPI]
 */

import {before, describe, it} from "jstests/libs/mochalite.js";

const OPTIMIZATION_ERROR_PREFIX = "Failed to optimize pipeline :: caused by :: ";
const EXTENSION_ERROR_PREFIX = "Extension encountered error: ";

const AssertionType = {
    UASSERT: "uassert",
    ERROR: "error",
};

const Phase = {
    OPT_PRECONDITION: "optimization_precondition",
    OPT_TRANSFORM: "optimization_transform",
};

function isOptimizationPhase(phase) {
    return phase === Phase.OPT_PRECONDITION || phase === Phase.OPT_TRANSFORM;
}

describe("extension error propagation", function () {
    const coll = db[jsTestName()];

    before(function () {
        coll.drop();
        assert.commandWorked(coll.insertMany([{x: 1}, {x: 2}, {x: 3}]));
    });

    function runAssert(assertArgs) {
        return db.runCommand({
            aggregate: coll.getName(),
            pipeline: [{$assert: assertArgs}],
            cursor: {},
        });
    }

    function checkAssertionFailure({errmsg, code, assertionType, assertInPhase}) {
        const res = assert.commandFailedWithCode(
            runAssert({errmsg, code, assertionType, assertInPhase}),
            code,
        );
        const baseErrMsg =
            assertionType === AssertionType.UASSERT ? errmsg : EXTENSION_ERROR_PREFIX + errmsg;
        const expectedErrMsg = isOptimizationPhase(assertInPhase)
            ? OPTIMIZATION_ERROR_PREFIX + baseErrMsg
            : baseErrMsg;
        assert.eq(res.errmsg, expectedErrMsg, res);
        return res;
    }

    function checkGenericError({assertInPhase} = {}) {
        const res = assert.commandFailed(
            runAssert({assertionType: AssertionType.ERROR, assertInPhase}),
        );
        // Generic C++ errors have an empty reason. addContext still appends " :: caused by :: " as a
        // separator even for empty reasons, so the optimization-phase errmsg ends with the separator.
        assert.eq(
            res.errmsg,
            isOptimizationPhase(assertInPhase) ? OPTIMIZATION_ERROR_PREFIX : "",
            res,
        );
        assert.eq(res.code, -1, res);
    }

    describe("parse-time paths", function () {
        const parseAssertionCases = [
            {
                name: "uassert with custom code",
                errmsg: "this is a fake uassert",
                code: 1234,
                assertionType: AssertionType.UASSERT,
                expectedCodeName: "Location1234",
            },
            {
                name: "uassert with BadValue code",
                errmsg: "this is a fake BadValue",
                code: ErrorCodes.BadValue,
                assertionType: AssertionType.UASSERT,
                expectedCodeName: "BadValue",
            },
        ];

        for (const testcase of parseAssertionCases) {
            it(testcase.name, function () {
                const res = checkAssertionFailure(testcase);
                assert.eq(res.codeName, testcase.expectedCodeName, res);
            });
        }

        it("generic error", function () {
            checkGenericError();
        });
    });

    describe("optimization-phase paths", function () {
        const optimizationAssertionCases = [
            {
                errmsg: "uassert in optimization precondition",
                code: 1234,
                assertionType: AssertionType.UASSERT,
                assertInPhase: Phase.OPT_PRECONDITION,
            },
            {
                errmsg: "uassert in optimization transform",
                code: 5678,
                assertionType: AssertionType.UASSERT,
                assertInPhase: Phase.OPT_TRANSFORM,
            },
        ];

        for (const testcase of optimizationAssertionCases) {
            it(`uassert in ${testcase.assertInPhase}`, function () {
                checkAssertionFailure(testcase);
            });
        }

        for (const assertInPhase of [Phase.OPT_PRECONDITION, Phase.OPT_TRANSFORM]) {
            it(`generic error in ${assertInPhase}`, function () {
                checkGenericError({assertInPhase});
            });
        }
    });
});
