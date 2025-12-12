/**
 * Tests for the assertion functions in mongo/shell/assert.js.
 */

import {describe, it} from "jstests/libs/mochalite.js";

const kDefaultTimeoutMS = 10 * 1000;
const kSmallTimeoutMS = 200;
const kSmallRetryIntervalMS = 1;
const kDefaultRetryAttempts = 5;
const kAttr = {
    attr1: "some attribute",
};

describe("doassert tests", function () {
    it("callingDoAssertWithStringThrowsException", function () {
        const expectedError = "hello world";
        const actualError = assert.throws(() => {
            doassert(expectedError);
        });

        assert.eq("Error: " + expectedError, actualError, "doAssert should throw passed msg as exception");
    });

    it("callingDoAssertWithObjectThrowsException", function () {
        const expectedError = {err: "hello world"};
        const actualError = assert.throws(() => {
            doassert(expectedError);
        });

        assert.eq("Error: " + tojson(expectedError), actualError, "doAssert should throw passed object as exception");
    });

    it("callingDoAssertWithStringPassedAsFunctionThrowsException", function () {
        const expectedError = "hello world";
        const actualError = assert.throws(() => {
            doassert(() => {
                return expectedError;
            });
        });

        assert.eq("Error: " + expectedError, actualError, "doAssert should throw passed msg as exception");
    });

    it("callingDoAssertWithObjectAsFunctionThrowsException", function () {
        const expectedError = {err: "hello world"};
        const actualError = assert.throws(() => {
            doassert(() => {
                return expectedError;
            });
        });

        assert.eq("Error: " + tojson(expectedError), actualError, "doAssert should throw passed object as exception");
    });

    it("callingDoAssertCorrectlyAttachesWriteErrors", function () {
        const errorMessage = "Operation was interrupted";
        const bulkResult = {
            nModified: 0,
            n: 0,
            writeErrors: [{"index": 0, "code": 11601, "errmsg": errorMessage}],
            upserted: [],
            ok: 1,
        };

        let error = assert.throws(() => doassert(errorMessage, new BulkWriteError(bulkResult)));
        assert.eq(error.writeErrors[0].code, bulkResult.writeErrors[0].code);

        error = assert.throws(() => doassert(errorMessage, new BulkWriteResult(bulkResult)));
        assert.eq(error.writeErrors[0].code, bulkResult.writeErrors[0].code);

        error = assert.throws(() => doassert(errorMessage, new WriteResult(bulkResult)));
        assert.eq(error.writeErrors[0].code, bulkResult.writeErrors[0].code);

        error = assert.throws(() => doassert(errorMessage, bulkResult));
        assert.eq(error.writeErrors[0].code, bulkResult.writeErrors[0].code);
    });

    it("callingDoAssertCorrectlyAttachesWriteConcernError", function () {
        const errorMessage = "Operation was interrupted";
        const bulkResult = {
            nModified: 0,
            n: 0,
            writeConcernErrors: [{code: 6, codeName: "HostUnreachable", errmsg: errorMessage, errInfo: {}}],
            upserted: [],
            ok: 1,
        };

        let error = assert.throws(() => doassert(errorMessage, new BulkWriteError(bulkResult)));
        assert.eq(error.writeConcernError.code, bulkResult.writeConcernErrors[0].code);

        error = assert.throws(() => doassert(errorMessage, new BulkWriteResult(bulkResult)));
        assert.eq(error.writeConcernError.code, bulkResult.writeConcernErrors[0].code);

        error = assert.throws(() => doassert(errorMessage, new WriteResult(bulkResult)));
        assert.eq(error.writeConcernError.code, bulkResult.writeConcernErrors[0].code);

        error = assert.throws(() => doassert(errorMessage, bulkResult));
        assert.eq(error.writeConcernError.code, bulkResult.writeConcernErrors[0].code);
    });
});

describe("assert tests", function () {
    it("assertShouldFailForMoreThan3Args", function () {
        const err = assert.throws(() => {
            assert(1, 2, 3, 4);
        });
        assert.neq(-1, err.message.indexOf("Too many parameters"), "Too many params message should be displayed");
    });

    it("assertShouldNotThrowExceptionForTrue", function () {
        assert.doesNotThrow(() => {
            assert(true, "message");
        });
    });

    it("assertShouldThrowExceptionForFalse", function () {
        const expectedMessage = "message";
        const err = assert.throws(() => {
            assert(false, expectedMessage);
        });

        assert.neq(-1, err.message.indexOf(expectedMessage), "assert message should be thrown on error");
    });

    it("assertShouldThrowExceptionForFalseWithDefaultMessage", function () {
        const defaultMessage = "assert failed";
        const err = assert.throws(() => {
            assert(false);
        });

        assert.eq(defaultMessage, err.message, "assert message should be thrown on error");
    });

    it("assertShouldThrowExceptionForFalseWithDefaultMessagePrefix", function () {
        const prefix = "assert failed";
        const message = "the assertion failed";
        const err = assert.throws(() => {
            assert(false, message);
        });

        assert.neq(-1, err.message.indexOf(prefix), "assert message should should contain prefix");
        assert.neq(-1, err.message.indexOf(message), "assert message should should contain original message");
    });

    it("assertShouldNotCallMsgFunctionsOnSuccess", function () {
        let called = false;

        assert(true, () => {
            called = true;
        });

        assert.eq(false, called, "called should not have been udpated");
    });

    it("assertShouldCallMsgFunctionsOnFailure", function () {
        let called = false;

        assert.throws(() => {
            assert(false, () => {
                called = true;
                return "error message";
            });
        });

        assert.eq(true, called, "called should not have been udpated");
    });

    it("assertShouldAcceptObjectAsMsg", function () {
        const objMsg = {someMessage: 1};
        const err = assert.throws(() => {
            assert(false, objMsg);
        });

        assert.neq(-1, err.message.indexOf(tojson(objMsg)), "Error message should have included " + tojson(objMsg));
    });

    it("assertShouldNotAcceptNonObjStringFunctionAsMsg", function () {
        const err = assert.throws(() => {
            assert(true, 1234);
        });

        assert.neq(-1, err.message.indexOf("msg parameter must be a "));
    });
});

describe("assert.eq tests", function () {
    it("eqShouldPassOnEquality", function () {
        assert.doesNotThrow(() => {
            assert.eq(3, 3);
        });
    });

    it("eqShouldFailWhenNotEqual", function () {
        assert.throws(() => {
            assert.eq(2, 3);
        });
    });

    it("eqShouldNotCallMsgFunctionOnSuccess", function () {
        let called = false;

        assert.doesNotThrow(() => {
            assert.eq(3, 3, () => {
                called = true;
            });
        });

        assert.eq(false, called, "msg function should not have been called");
    });

    it("eqShouldCallMsgFunctionOnFailure", function () {
        let called = false;

        assert.throws(() => {
            assert.eq(1, 3, () => {
                called = true;
            });
        });

        assert.eq(true, called, "msg function should have been called");
    });

    it("eqShouldPassOnObjectsWithSameContent", function () {
        const a = {"foo": true};
        const b = {"foo": true};

        assert.doesNotThrow(
            () => {
                assert.eq(a, b);
            },
            [],
            "eq should not throw exception on two objects with the same content",
        );
    });
});

describe("assert.neq tests", function () {
    it("neqShouldFailOnEquality", function () {
        assert.throws(() => {
            assert.neq(3, 3);
        });
    });

    it("neqShouldPassWhenNotEqual", function () {
        assert.doesNotThrow(() => {
            assert.neq(2, 3);
        });
    });

    it("neqShouldFailOnObjectsWithSameContent", function () {
        const a = {"foo": true};
        const b = {"foo": true};

        assert.throws(
            () => {
                assert.neq(a, b);
            },
            [],
            "neq should throw exception on two objects with the same content",
        );
    });
});

describe("assert.hasFields tests", function () {
    it("hasFieldsRequiresAnArrayOfFields", function () {
        const object = {field1: 1, field2: 1, field3: 1};

        assert.throws(() => {
            assert.hasFields(object, "field1");
        });
    });

    it("hasFieldsShouldPassWhenObjectHasField", function () {
        const object = {field1: 1, field2: 1, field3: 1};

        assert.doesNotThrow(() => {
            assert.hasFields(object, ["field1"]);
        });
    });

    it("hasFieldsShouldFailWhenObjectDoesNotHaveField", function () {
        const object = {field1: 1, field2: 1, field3: 1};

        assert.throws(() => {
            assert.hasFields(object, ["fieldDoesNotExist"]);
        });
    });

    /* assert.contains tests */

    it("containsShouldOnlyWorkOnArrays", function () {
        assert.throws(() => {
            assert.contains(42, 5);
        });
    });

    it("containsShouldPassIfArrayContainsValue", function () {
        const array = [1, 2, 3];

        assert.doesNotThrow(() => {
            assert.contains(2, array);
        });
    });

    it("containsShouldFailIfArrayDoesNotContainValue", function () {
        const array = [1, 2, 3];

        assert.throws(() => {
            assert.contains(42, array);
        });
    });
});

describe("assert.soon tests", function () {
    it("soonPassesWhenFunctionPasses", function () {
        assert.doesNotThrow(() => {
            assert.soon(() => {
                return true;
            });
        });
    });

    it("soonFailsIfMethodNeverPasses", function () {
        assert.throws(() => {
            assert.soon(
                () => {
                    return false;
                },
                "assert message",
                kSmallTimeoutMS,
                kSmallRetryIntervalMS,
                {runHangAnalyzer: false},
            );
        });
    });

    it("soonPassesIfMethodEventuallyPasses", function () {
        let count = 0;
        assert.doesNotThrow(() => {
            assert.soon(
                () => {
                    count += 1;
                    return count === 3;
                },
                "assert message",
                kDefaultTimeoutMS,
                kSmallRetryIntervalMS,
            );
        });
    });
});

describe("assert.soonNoExcept tests", function () {
    it("soonNoExceptEventuallyPassesEvenWithExceptions", function () {
        let count = 0;
        assert.doesNotThrow(() => {
            assert.soonNoExcept(
                () => {
                    count += 1;
                    if (count < 3) {
                        throw new Error("failed");
                    }
                    return true;
                },
                "assert message",
                kDefaultTimeoutMS,
                kSmallRetryIntervalMS,
            );
        });
    });

    it("soonNoExceptFailsIfExceptionAlwaysThrown", function () {
        assert.throws(() => {
            assert.soonNoExcept(
                () => {
                    throw new Error("failed");
                },
                "assert message",
                kSmallTimeoutMS,
                kSmallRetryIntervalMS,
                {runHangAnalyzer: false},
            );
        });
    });
});

describe("assert.retry tests", function () {
    it("retryPassesAfterAFewAttempts", function () {
        let count = 0;

        assert.doesNotThrow(() => {
            assert.retry(
                () => {
                    count += 1;
                    return count === 3;
                },
                "assert message",
                kDefaultRetryAttempts,
                kSmallRetryIntervalMS,
            );
        });
    });

    it("retryFailsAfterMaxAttempts", function () {
        assert.throws(() => {
            assert.retry(
                () => {
                    return false;
                },
                "assert message",
                kDefaultRetryAttempts,
                kSmallRetryIntervalMS,
                {
                    runHangAnalyzer: false,
                },
            );
        });
    });
});

describe("assert.retryNoExcept tests", function () {
    it("retryNoExceptPassesAfterAFewAttempts", function () {
        let count = 0;

        assert.doesNotThrow(() => {
            assert.retryNoExcept(
                () => {
                    count += 1;
                    if (count < 3) {
                        throw new Error("failed");
                    }
                    return count === 3;
                },
                "assert message",
                kDefaultRetryAttempts,
                kSmallRetryIntervalMS,
            );
        });
    });

    it("retryNoExceptFailsAfterMaxAttempts", function () {
        assert.throws(() => {
            assert.retryNoExcept(
                () => {
                    throw new Error("failed");
                },
                "assert message",
                kDefaultRetryAttempts,
                kSmallRetryIntervalMS,
                {
                    runHangAnalyzer: false,
                },
            );
        });
    });
});

describe("assert.time tests", function () {
    it("timeIsSuccessfulIfFuncExecutesInTime", function () {
        assert.doesNotThrow(() => {
            assert.time(
                () => {
                    return true;
                },
                "assert message",
                kDefaultTimeoutMS,
            );
        });
    });

    it("timeFailsIfFuncDoesNotFinishInTime", function () {
        assert.throws(() => {
            assert.time(
                () => {
                    return true;
                },
                "assert message",
                -5 * 60 * 1000,
                {runHangAnalyzer: false},
            );
        });
    });
});

describe("assert.isnull tests", function () {
    it("isnullPassesOnNull", function () {
        assert.doesNotThrow(() => {
            assert.isnull(null);
        });
    });

    it("isnullPassesOnUndefined", function () {
        assert.doesNotThrow(() => {
            assert.isnull(undefined);
        });
    });

    it("isnullFailsOnNotNull", function () {
        assert.throws(() => {
            assert.isnull("hello world");
        });
    });
});

describe("assert.lt tests", function () {
    it("ltPassesWhenLessThan", function () {
        assert.doesNotThrow(() => {
            assert.lt(3, 5);
        });
    });

    it("ltFailsWhenNotLessThan", function () {
        assert.throws(() => {
            assert.lt(5, 3);
        });
    });

    it("ltFailsWhenEqual", function () {
        assert.throws(() => {
            assert.lt(5, 5);
        });
    });

    it("ltPassesWhenLessThanWithTimestamps", function () {
        assert.doesNotThrow(() => {
            assert.lt(Timestamp(3, 0), Timestamp(10, 0));
        });
    });

    it("ltFailsWhenNotLessThanWithTimestamps", function () {
        assert.throws(() => {
            assert.lt(Timestamp(0, 10), Timestamp(0, 3));
        });
    });

    it("ltFailsWhenEqualWithTimestamps", function () {
        assert.throws(() => {
            assert.lt(Timestamp(5, 0), Timestamp(5, 0));
        });
    });
});

describe("assert.gt tests", function () {
    it("gtPassesWhenGreaterThan", function () {
        assert.doesNotThrow(() => {
            assert.gt(5, 3);
        });
    });

    it("gtFailsWhenNotGreaterThan", function () {
        assert.throws(() => {
            assert.gt(3, 5);
        });
    });

    it("gtFailsWhenEqual", function () {
        assert.throws(() => {
            assert.gt(5, 5);
        });
    });
});

describe("assert.lte tests", function () {
    it("ltePassesWhenLessThan", function () {
        assert.doesNotThrow(() => {
            assert.lte(3, 5);
        });
    });

    it("lteFailsWhenNotLessThan", function () {
        assert.throws(() => {
            assert.lte(5, 3);
        });
    });

    it("ltePassesWhenEqual", function () {
        assert.doesNotThrow(() => {
            assert.lte(5, 5);
        });
    });
});

describe("assert.gte tests", function () {
    it("gtePassesWhenGreaterThan", function () {
        assert.doesNotThrow(() => {
            assert.gte(5, 3);
        });
    });

    it("gteFailsWhenNotGreaterThan", function () {
        assert.throws(() => {
            assert.gte(3, 5);
        });
    });

    it("gtePassesWhenEqual", function () {
        assert.doesNotThrow(() => {
            assert.gte(5, 5);
        });
    });

    it("gtePassesWhenGreaterThanWithTimestamps", function () {
        assert.doesNotThrow(() => {
            assert.gte(Timestamp(0, 10), Timestamp(0, 3));
        });
    });

    it("gteFailsWhenNotGreaterThanWithTimestamps", function () {
        assert.throws(() => {
            assert.gte(Timestamp(0, 3), Timestamp(0, 10));
        });
    });

    it("gtePassesWhenEqualWIthTimestamps", function () {
        assert.doesNotThrow(() => {
            assert.gte(Timestamp(5, 0), Timestamp(5, 0));
        });
    });
});

describe("assert.betweenIn tests", function () {
    it("betweenInPassWhenNumberIsBetween", function () {
        assert.doesNotThrow(() => {
            assert.betweenIn(3, 4, 5);
        });
    });

    it("betweenInFailsWhenNumberIsNotBetween", function () {
        assert.throws(() => {
            assert.betweenIn(3, 5, 4);
        });
    });

    it("betweenInPassWhenNumbersEqual", function () {
        assert.doesNotThrow(() => {
            assert.betweenIn(3, 3, 5);
        });
        assert.doesNotThrow(() => {
            assert.betweenIn(3, 5, 5);
        });
    });
});

describe("assert.betweenEx tests", function () {
    it("betweenExPassWhenNumberIsBetween", function () {
        assert.doesNotThrow(() => {
            assert.betweenEx(3, 4, 5);
        });
    });

    it("betweenExFailsWhenNumberIsNotBetween", function () {
        assert.throws(() => {
            assert.betweenEx(3, 5, 4);
        });
    });

    it("betweenExFailsWhenNumbersEqual", function () {
        assert.throws(() => {
            assert.betweenEx(3, 3, 5);
        });
        assert.throws(() => {
            assert.betweenEx(3, 5, 5);
        });
    });
});

describe("assert.sameMembers tests", function () {
    it("sameMembersFailsWithInvalidArguments", function () {
        assert.throws(() => assert.sameMembers());
        assert.throws(() => assert.sameMembers([]));
        assert.throws(() => assert.sameMembers({}, {}));
        assert.throws(() => assert.sameMembers(1, 1));
    });

    it("sameMembersFailsWhenLengthsDifferent", function () {
        assert.throws(() => assert.sameMembers([], [1]));
        assert.throws(() => assert.sameMembers([], [1]));
        assert.throws(() => assert.sameMembers([1, 2], [1]));
        assert.throws(() => assert.sameMembers([1], [1, 2]));
    });

    it("sameMembersFailsWhenCountsOfDuplicatesDifferent", function () {
        assert.throws(() => assert.sameMembers([1, 1], [1, 2]));
        assert.throws(() => assert.sameMembers([1, 2], [1, 1]));
    });

    it("sameMembersFailsWithDifferentObjects", function () {
        assert.throws(() => assert.sameMembers([{_id: 0, a: 0}], [{_id: 0, a: 1}]));
        assert.throws(() => assert.sameMembers([{_id: 1, a: 0}], [{_id: 0, a: 0}]));
        assert.throws(() => {
            assert.sameMembers([{a: [{b: 0, c: 0}], _id: 0}], [{_id: 0, a: [{c: 0, b: 1}]}]);
        });
    });

    it("sameMembersFailsWithDifferentBSONTypes", function () {
        assert.throws(() => {
            assert.sameMembers(
                [new BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq")],
                [new BinData(0, "xxxgqwetkqwklEWRbWERKKJREtbq")],
            );
        });
        assert.throws(() => assert.sameMembers([new Timestamp(0, 1)], [new Timestamp(0, 2)]));
    });

    it("sameMembersFailsWithCustomCompareFn", function () {
        const compareBinaryEqual = (a, b) => bsonBinaryEqual(a, b);
        assert.throws(() => {
            assert.sameMembers([NumberLong(1)], [1], undefined /*msg*/, compareBinaryEqual);
        });
        assert.throws(() => {
            assert.sameMembers(
                [NumberLong(1), NumberInt(2)],
                [2, NumberLong(1)],
                undefined /*msg*/,
                compareBinaryEqual,
            );
        });
    });

    it("sameMembersDoesNotSortNestedArrays", function () {
        assert.throws(() => assert.sameMembers([[1, 2]], [[2, 1]]));
        assert.throws(() => {
            assert.sameMembers([{a: [{b: 0}, {b: 1, c: 0}], _id: 0}], [{_id: 0, a: [{c: 0, b: 1}, {b: 0}]}]);
        });
    });

    it("sameMembersPassesWithEmptyArrays", function () {
        assert.sameMembers([], []);
    });

    it("sameMembersPassesSingleElement", function () {
        assert.sameMembers([1], [1]);
    });

    it("sameMembersPassesWithSameOrder", function () {
        assert.sameMembers([1, 2], [1, 2]);
        assert.sameMembers([1, 2, 3], [1, 2, 3]);
    });

    it("sameMembersPassesWithDifferentOrder", function () {
        assert.sameMembers([2, 1], [1, 2]);
        assert.sameMembers([1, 2, 3], [3, 1, 2]);
    });

    it("sameMembersPassesWithDuplicates", function () {
        assert.sameMembers([1, 1, 2], [1, 1, 2]);
        assert.sameMembers([1, 1, 2], [1, 2, 1]);
        assert.sameMembers([2, 1, 1], [1, 1, 2]);
    });

    it("sameMembersPassesWithSortedNestedArrays", function () {
        assert.sameMembers([[1, 2]], [[1, 2]]);
        assert.sameMembers([{a: [{b: 0}, {b: 1, c: 0}], _id: 0}], [{_id: 0, a: [{b: 0}, {c: 0, b: 1}]}]);
    });

    it("sameMembersPassesWithObjects", function () {
        assert.sameMembers([{_id: 0, a: 0}], [{_id: 0, a: 0}]);
        assert.sameMembers([{_id: 0, a: 0}, {_id: 1}], [{_id: 0, a: 0}, {_id: 1}]);
        assert.sameMembers([{_id: 0, a: 0}, {_id: 1}], [{_id: 1}, {_id: 0, a: 0}]);
    });

    it("sameMembersPassesWithUnsortedObjects", function () {
        assert.sameMembers([{a: 0, _id: 1}], [{_id: 1, a: 0}]);
        assert.sameMembers([{a: [{b: 1, c: 0}], _id: 0}], [{_id: 0, a: [{c: 0, b: 1}]}]);
    });

    it("sameMembersPassesWithBSONTypes", function () {
        assert.sameMembers(
            [new BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq")],
            [new BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq")],
        );
        assert.sameMembers([new Timestamp(0, 1)], [new Timestamp(0, 1)]);
    });

    it("sameMembersPassesWithOtherTypes", function () {
        assert.sameMembers([null], [null]);
        assert.sameMembers([undefined], [undefined]);
        assert.sameMembers(["a"], ["a"]);
        assert.sameMembers([null, undefined, "a"], [undefined, "a", null]);
    });

    it("sameMembersDefaultCompareIsFriendly", function () {
        assert.sameMembers([NumberLong(1), NumberInt(2)], [2, 1]);
    });

    it("sameMembersPassesWithCustomCompareFn", function () {
        const compareBinaryEqual = (a, b) => bsonBinaryEqual(a, b);
        assert.sameMembers([[1, 2]], [[1, 2]], undefined /*msg*/, compareBinaryEqual);
        assert.sameMembers(
            [NumberLong(1), NumberInt(2)],
            [NumberInt(2), NumberLong(1)],
            undefined /*msg*/,
            compareBinaryEqual,
        );
    });
});

it("assertCallsHangAnalyzer", function () {
    function runAssertTest(f) {
        const oldMongoRunner = MongoRunner;
        let runs = 0;
        try {
            MongoRunner.runHangAnalyzer = function () {
                ++runs;
            };
            f();
            assert(false);
        } catch (e) {
            assert.eq(runs, 1);
        } finally {
            MongoRunner = oldMongoRunner;
        }
    }
    runAssertTest(() => assert.soon(() => false, "assert message", kSmallTimeoutMS, kSmallRetryIntervalMS));
    runAssertTest(() => assert.retry(() => false, "assert message", kDefaultRetryAttempts, kSmallRetryIntervalMS));
    runAssertTest(() => assert.time(() => sleep(5), "assert message", 1 /* we certainly take less than this */));
});

function assertThrowsErrorWithJson(assertFailureTriggerFn, {msg, attr}) {
    const oldLogFormat = TestData.logFormat;
    try {
        TestData.logFormat = "json";
        assertFailureTriggerFn();
    } catch (e) {
        assert.eq(msg, e.message, "unexpected error message");
        assert.eq(toJsonForLog(attr), toJsonForLog(e.extraAttr), "unexpected extra attributes");
    } finally {
        TestData.logFormat = oldLogFormat;
    }
    // Call the 'assertFailureTriggerFn' second time to make sure it actually throws.
    assert.throws(assertFailureTriggerFn, [], "assertFailureTriggerFn");
}

it("assertJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert(false, "lorem ipsum");
        },
        {msg: "assert failed : lorem ipsum"},
    );
    assertThrowsErrorWithJson(
        () => {
            assert(false, "lorem ipsum", kAttr);
        },
        {msg: "assert failed : lorem ipsum", attr: {...kAttr}},
    );
});

it("assertEqJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.eq(5, 2 + 2, "lorem ipsum");
        },
        {
            msg: `\
expected 5 to equal 4
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

\u001b[31m-5\u001b[0m
\u001b[32m+4\u001b[0m
 : lorem ipsum`,
            attr: {},
        },
    );
    assertThrowsErrorWithJson(
        () => {
            assert.eq(5, 2 + 2, "lorem ipsum", kAttr);
        },
        {
            msg: `\
expected 5 to equal 4
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

\u001b[31m-5\u001b[0m
\u001b[32m+4\u001b[0m
 : lorem ipsum`,
            attr: {...kAttr},
        },
    );

    assertThrowsErrorWithJson(
        () => {
            assert.eq([["a", "c"]], [["a", "b", "c"]]);
        },
        {
            msg: `\
expected [ [Array] ] to equal [ [Array] ]
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

\u001b[31m-[ [ "a", "c" ] ]\u001b[0m
\u001b[32m+[ [ "a", "b", "c" ] ]\u001b[0m
`,
            attr: {},
        },
    );
    assertThrowsErrorWithJson(
        () => {
            assert.eq([{a: 1, c: 3}], [{a: 1, b: 2, c: 3}]);
        },
        {
            msg: `\
expected [ [Object] ] to equal [ [Object] ]
\u001b[32m+ expected\u001b[0m \u001b[31m- actual\u001b[0m

\u001b[31m-[ { "a" : 1, "c" : 3 } ]\u001b[0m
\u001b[32m+[ { "a" : 1, "b" : 2, "c" : 3 } ]\u001b[0m
`,
            attr: {},
        },
    );
});

it("assertDocEqJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.docEq({msg: "hello"}, {msg: "goodbye"}, "lorem ipsum", kAttr);
        },
        {
            msg: "expected document {expectedDoc} and actual document {actualDoc} are not equal : lorem ipsum",
            attr: {expectedDoc: {msg: "hello"}, actualDoc: {msg: "goodbye"}, ...kAttr},
        },
    );
});

it("assertSetEqJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.setEq(new Set([1, 2, 3]), new Set([4, 5]), "lorem ipsum", kAttr);
        },
        {
            msg: "expected set {expectedSet} and actual set {actualSet} are not equal : lorem ipsum",
            attr: {expectedSet: [1, 2, 3], actualSet: [4, 5], ...kAttr},
        },
    );
});

it("assertSameMembersJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.sameMembers([1, 2], [1], "Oops!", assert._isDocEq, kAttr);
        },
        {
            msg: "{aArr} != {bArr} : Oops!",
            attr: {aArr: [1, 2], bArr: [1], compareFn: "_isDocEq", ...kAttr},
        },
    );
});

it("assertFuzzySameMembersJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.fuzzySameMembers([{soccer: 42}], [{score: 42000}], ["score"], "Oops!", 4, kAttr);
        },
        {
            msg: "{aArr} != {bArr} : Oops!",
            attr: {aArr: [{soccer: 42}], bArr: [{score: 42000}], compareFn: "fuzzyCompare", ...kAttr},
        },
    );
});

it("assertNeqJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.neq(42, 42, "Oops!", kAttr);
        },
        {msg: "[{a}] and [{b}] are equal : Oops!", attr: {a: 42, b: 42, ...kAttr}},
    );
});

it("assertHasFieldsJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.hasFields({hello: "world"}, ["goodbye"], "Oops!", kAttr);
        },
        {
            msg: "Not all of the values from {arr} were in {obj} : Oops!",
            attr: {obj: {hello: "world"}, arr: ["goodbye"], ...kAttr},
        },
    );
});

it("assertContainsJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.contains(3, [14, 15, 926], "Oops!", kAttr);
        },
        {
            msg: "{element} was not in {arr} : Oops!",
            attr: {element: 3, arr: [14, 15, 926], ...kAttr},
        },
    );
});

it("assertDoesNotContainJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.doesNotContain(3, [3, 23], "Oops!", kAttr);
        },
        {msg: "{element} is in {arr} : Oops!", attr: {element: 3, arr: [3, 23], ...kAttr}},
    );
});

it("assertContainsPrefixJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.containsPrefix("hello", ["hell", "help"], "Oops!", kAttr);
        },
        {
            msg: "{prefix} was not a prefix in {arr} : Oops!",
            attr: {prefix: "hello", arr: ["hell", "help"], ...kAttr},
        },
    );
});

it("assertSoonJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.soon(() => false, "Oops!", 20, 10, {runHangAnalyzer: false}, kAttr);
        },
        {msg: "assert.soon failed (timeout 20ms), msg : Oops!", attr: {...kAttr}},
    );
});

it("assertSoonNoExceptJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.soonNoExcept(
                () => {
                    throw Error("disaster");
                },
                "Oops!",
                20,
                10,
                {runHangAnalyzer: false},
                kAttr,
            );
        },
        {msg: "assert.soon failed (timeout 20ms), msg : Oops!", attr: {...kAttr}},
    );
});

it("assertRetryJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.retry(() => false, "Oops!", 2, 10, {runHangAnalyzer: false}, kAttr);
        },
        {msg: "Oops!", attr: {...kAttr}},
    );
});

it("assertRetryNoExceptJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.retryNoExcept(
                () => {
                    throw Error("disaster");
                },
                "Oops!",
                2,
                10,
                {runHangAnalyzer: false},
                kAttr,
            );
        },
        {msg: "Oops!", attr: {...kAttr}},
    );
});

it("assertTimeJsonFormat", function () {
    const sleepTimeMS = 20,
        timeoutMS = 10;
    const f = () => {
        sleep(sleepTimeMS);
    };
    assertThrowsErrorWithJson(
        () => {
            try {
                assert.time(f, "Oops!", timeoutMS, {runHangAnalyzer: false}, kAttr);
            } catch (e) {
                // Override 'timeMS' to make the test deterministic.
                e.extraAttr.timeMS = sleepTimeMS;
                // Override 'diff' to make the test deterministic.
                e.extraAttr.diff = sleepTimeMS;
                throw e;
            }
        },
        {
            msg: "assert.time failed : Oops!",
            attr: {timeMS: sleepTimeMS, timeoutMS, function: f, diff: sleepTimeMS, ...kAttr},
        },
    );
});

it("assertThrowsJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.throws(() => true, [], "Oops!", kAttr);
        },
        {msg: "did not throw exception : Oops!", attr: {...kAttr}},
    );
});

it("assertThrowsWithCodeJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            const err = new Error("disaster");
            err.code = 24;
            assert.throwsWithCode(
                () => {
                    throw err;
                },
                42,
                [],
                "Oops!",
                kAttr,
            );
        },
        {
            msg: "[{code}] and [{expectedCode}] are not equal : Oops!",
            attr: {code: 24, expectedCode: [42], ...kAttr},
        },
    );
});

it("assertDoesNotThrowJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            const err = new Error("disaster");
            assert.doesNotThrow(
                () => {
                    throw err;
                },
                [],
                "Oops!",
                kAttr,
            );
        },
        {
            msg: "threw unexpected exception: {error} : Oops!",
            attr: {error: {message: "disaster"}, ...kAttr},
        },
    );
});

it("assertCommandWorkedWrongArgumentTypeJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.commandWorked("cmd", "Oops!");
        },
        {
            msg: "expected result type 'object', got '{resultType}', res='{result}' : unexpected result type given to assert.commandWorked()",
            attr: {result: "cmd", resultType: "string"},
        },
    );
});

it("assertCommandWorkedJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            const res = {
                ok: 0,
                code: ErrorCodes.BadValue,
                codeName: ErrorCodeStrings[ErrorCodes.BadValue],
                errmsg: "unexpected error",
                _mongo: "connection to localhost:20000",
                _commandObj: {hello: 1},
            };
            assert.commandWorked(res, "Oops!");
        },
        {
            msg: "command failed: {res} with original command request: {originalCommand} with errmsg: unexpected error : Oops!",
            attr: {
                res: {
                    ok: 0,
                    code: ErrorCodes.BadValue,
                    codeName: ErrorCodeStrings[ErrorCodes.BadValue],
                    errmsg: "unexpected error",
                    _mongo: "connection to localhost:20000", // Will not be seen when 'res' is BSON.
                    _commandObj: {hello: 1}, // Will not be seen when 'res' is BSON.
                },
                originalCommand: {hello: 1},
                connection: "connection to localhost:20000",
            },
        },
    );
});

it("assertCommandFailedJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            const res = {ok: 1, _mongo: "connection to localhost:20000", _commandObj: {hello: 1}};
            assert.commandFailed(res, "Oops!");
        },
        {
            msg: "command worked when it should have failed: {res} : Oops!",
            attr: {
                res: {
                    ok: 1,
                    _mongo: "connection to localhost:20000", // Will not be seen when 'res' is BSON.
                    _commandObj: {hello: 1}, // Will not be seen when 'res' is BSON.
                },
                originalCommand: {hello: 1},
                connection: "connection to localhost:20000",
            },
        },
    );
});

it("assertCommandFailedWithCodeJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            const res = {ok: 1, _mongo: "connection to localhost:20000", _commandObj: {hello: 1}};
            assert.commandFailedWithCode(res, ErrorCodes.BadValue, "Oops!");
        },
        {
            msg: "command worked when it should have failed: {res} : Oops!",
            attr: {
                res: {
                    ok: 1,
                    _mongo: "connection to localhost:20000", // Will not be seen when 'res' is BSON.
                    _commandObj: {hello: 1}, // Will not be seen when 'res' is BSON.
                },
                originalCommand: {hello: 1},
                connection: "connection to localhost:20000",
            },
        },
    );
});

it("assertCommandFailedWithWrongCodeJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            const res = {
                ok: 0,
                code: ErrorCodes.BadValue,
                codeName: ErrorCodeStrings[ErrorCodes.BadValue],
                errmsg: "unexpected error",
                _mongo: "connection to localhost:20000",
                _commandObj: {hello: 1},
            };
            assert.commandFailedWithCode(res, ErrorCodes.NetworkTimeout, "Oops!");
        },
        {
            msg: "command did not fail with any of the following codes {expectedCode} {res}. errmsg: unexpected error : Oops!",
            attr: {
                res: {
                    ok: 0,
                    code: ErrorCodes.BadValue,
                    codeName: ErrorCodeStrings[ErrorCodes.BadValue],
                    errmsg: "unexpected error",
                    _mongo: "connection to localhost:20000", // Will not be seen when 'res' is BSON.
                    _commandObj: {hello: 1}, // Will not be seen when 'res' is BSON.
                },
                expectedCode: [89],
                originalCommand: {hello: 1},
                connection: "connection to localhost:20000",
            },
        },
    );
});

it("assertWriteOKJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            const res = {ok: 0};
            assert.writeOK(res, "Oops!");
        },
        {msg: "unknown type of write result, cannot check ok: {res} : Oops!", attr: {res: {ok: 0}}},
    );
});

it("assertWriteErrorJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            const res = new WriteResult({nRemoved: 0, writeErrors: [], upserted: []}, 3, {w: "majority"});
            assert.writeError(res, "Oops!");
        },
        {
            msg: "no write error: {res} : Oops!",
            attr: {
                res: {
                    ok: {"$undefined": true},
                    nInserted: {"$undefined": true},
                    nUpserted: {"$undefined": true},
                    nMatched: {"$undefined": true},
                    nModified: {"$undefined": true},
                    nRemoved: 0,
                },
            },
        },
    );
});

it("assertWriteErrorToJsonHasCorrectFields", function () {
    // Verify that the json obj has the correct name for the writeErrors field ('writeErrors' instead of 'writeError').
    const writeError = {code: ErrorCodes.NetworkTimeout, errmsg: "Timeout!"};
    const res = new WriteResult({nRemoved: 0, writeErrors: [writeError], upserted: []}, 3, {w: "majority"});
    const jsonRes = JSON.parse(tojson(res));

    assert(jsonRes.hasOwnProperty("writeErrors"));
    assert.eq(jsonRes.writeErrors, writeError);
    assert.eq(jsonRes.writeError, null);
});

it("assertWriteErrorWithCodeJsonFormat", function () {
    const writeError = {code: ErrorCodes.NetworkTimeout, errmsg: "Timeout!"};
    assertThrowsErrorWithJson(
        () => {
            const res = new WriteResult({nRemoved: 0, writeErrors: [writeError], upserted: []}, 3, {w: "majority"});
            assert.writeErrorWithCode(res, ErrorCodes.BadValue, "Oops!");
        },
        {
            msg: "found code(s) {writeErrorCodes} does not match any of the expected codes {expectedCode}. Original command response: {res} : Oops!",
            attr: {
                res: {
                    ok: {"$undefined": true},
                    nInserted: {"$undefined": true},
                    nUpserted: {"$undefined": true},
                    nMatched: {"$undefined": true},
                    nModified: {"$undefined": true},
                    nRemoved: 0,
                },
                expectedCode: [ErrorCodes.BadValue],
                writeErrorCodes: [ErrorCodes.NetworkTimeout],
            },
        },
    );
});

it("assertIsNullJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.isnull({ok: 1}, "Oops!", kAttr);
        },
        {msg: "supposed to be null, was: {value} : Oops!", attr: {value: {ok: 1}, ...kAttr}},
    );
});

it("assertLTJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.lt(41, 18, "Oops!", kAttr);
        },
        {msg: "{a} is not less than {b} : Oops!", attr: {a: 41, b: 18, ...kAttr}},
    );
});

it("assertBetweenJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.between(1, 15, 10, "Oops!", true, kAttr);
        },
        {
            msg: "{b} is not between {a} and {c} : Oops!",
            attr: {a: 1, b: 15, c: 10, inclusive: true, ...kAttr},
        },
    );
});

it("assertCloseJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.close(123.4567, 123.4678, "Oops!");
        },
        {
            msg: "123.4567 is not equal to 123.4678 within 4 places, absolute error: 0.011099999999999, relative error: 0.00008990198254118888 : Oops!",
        },
    );
});

it("assertCloseWithinMSJsonFormat", function () {
    const dateForLog = (arg) => JSON.parse(JSON.stringify(arg));
    const date1 = Date.UTC(1970, 0, 1, 23, 59, 59, 999);
    const date2 = date1 + 10;
    assertThrowsErrorWithJson(
        () => {
            assert.closeWithinMS(date1, date2, "Oops!", 1, kAttr);
        },
        {
            msg: "86399999 is not equal to 86400009 within 1 millis, actual delta: 10 millis : Oops!",
            attr: {a: dateForLog(date1), b: dateForLog(date2), deltaMS: 1, ...kAttr},
        },
    );
});

it("assertIncludesJsonFormat", function () {
    assertThrowsErrorWithJson(
        () => {
            assert.includes("farmacy", "ace", "Oops!", kAttr);
        },
        {
            msg: "string [{haystack}] does not include [{needle}] : Oops!",
            attr: {haystack: "farmacy", needle: "ace", ...kAttr},
        },
    );
});

it("assertIgnoreNonObjectExtraAttr", function () {
    const err = new Error("Oops!");
    err.extraAttr = "not an object";
    err.code = ErrorCodes.JSInterpreterFailure;
    assert.throwsWithCode(() => {
        throw err;
    }, ErrorCodes.JSInterpreterFailure);
});
