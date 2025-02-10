/**
 * Tests for the assertion functions in mongo/shell/assert.js.
 */
const tests = [];

const kDefaultTimeoutMS = 10 * 1000;
const kSmallTimeoutMS = 200;
const kSmallRetryIntervalMS = 1;
const kDefaultRetryAttempts = 5;
const kAttr = {
    attr1: "some attribute"
};

/* doassert tests */

tests.push(function callingDoAssertWithStringThrowsException() {
    const expectedError = 'hello world';
    const actualError = assert.throws(() => {
        doassert(expectedError);
    });

    assert.eq(
        'Error: ' + expectedError, actualError, 'doAssert should throw passed msg as exception');
});

tests.push(function callingDoAssertWithObjectThrowsException() {
    const expectedError = {err: 'hello world'};
    const actualError = assert.throws(() => {
        doassert(expectedError);
    });

    assert.eq('Error: ' + tojson(expectedError),
              actualError,
              'doAssert should throw passed object as exception');
});

tests.push(function callingDoAssertWithStringPassedAsFunctionThrowsException() {
    const expectedError = 'hello world';
    const actualError = assert.throws(() => {
        doassert(() => {
            return expectedError;
        });
    });

    assert.eq(
        'Error: ' + expectedError, actualError, 'doAssert should throw passed msg as exception');
});

tests.push(function callingDoAssertWithObjectAsFunctionThrowsException() {
    const expectedError = {err: 'hello world'};
    const actualError = assert.throws(() => {
        doassert(() => {
            return expectedError;
        });
    });

    assert.eq('Error: ' + tojson(expectedError),
              actualError,
              'doAssert should throw passed object as exception');
});

tests.push(function callingDoAssertCorrectlyAttachesWriteErrors() {
    const errorMessage = "Operation was interrupted";
    const bulkResult = {
        nModified: 0,
        n: 0,
        writeErrors: [{"index": 0, "code": 11601, "errmsg": errorMessage}],
        upserted: [],
        ok: 1
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

tests.push(function callingDoAssertCorrectlyAttachesWriteConcernError() {
    const errorMessage = "Operation was interrupted";
    const bulkResult = {
        nModified: 0,
        n: 0,
        writeConcernErrors:
            [{code: 6, codeName: "HostUnreachable", errmsg: errorMessage, errInfo: {}}],
        upserted: [],
        ok: 1
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

/* assert tests */

tests.push(function assertShouldFailForMoreThan3Args() {
    const err = assert.throws(() => {
        assert(1, 2, 3, 4);
    });
    assert.neq(-1,
               err.message.indexOf('Too many parameters'),
               'Too many params message should be displayed');
});

tests.push(function assertShouldNotThrowExceptionForTrue() {
    assert.doesNotThrow(() => {
        assert(true, 'message');
    });
});

tests.push(function assertShouldThrowExceptionForFalse() {
    const expectedMessage = 'message';
    const err = assert.throws(() => {
        assert(false, expectedMessage);
    });

    assert.neq(
        -1, err.message.indexOf(expectedMessage), 'assert message should be thrown on error');
});

tests.push(function assertShouldThrowExceptionForFalseWithDefaultMessage() {
    const defaultMessage = 'assert failed';
    const err = assert.throws(() => {
        assert(false);
    });

    assert.eq(defaultMessage, err.message, 'assert message should be thrown on error');
});

tests.push(function assertShouldThrowExceptionForFalseWithDefaultMessagePrefix() {
    const prefix = 'assert failed';
    const message = 'the assertion failed';
    const err = assert.throws(() => {
        assert(false, message);
    });

    assert.neq(-1, err.message.indexOf(prefix), 'assert message should should contain prefix');
    assert.neq(
        -1, err.message.indexOf(message), 'assert message should should contain original message');
});

tests.push(function assertShouldNotCallMsgFunctionsOnSuccess() {
    var called = false;

    assert(true, () => {
        called = true;
    });

    assert.eq(false, called, 'called should not have been udpated');
});

tests.push(function assertShouldCallMsgFunctionsOnFailure() {
    var called = false;

    assert.throws(() => {
        assert(false, () => {
            called = true;
            return 'error message';
        });
    });

    assert.eq(true, called, 'called should not have been udpated');
});

tests.push(function assertShouldAcceptObjectAsMsg() {
    const objMsg = {someMessage: 1};
    const err = assert.throws(() => {
        assert(false, objMsg);
    });

    assert.neq(-1,
               err.message.indexOf(tojson(objMsg)),
               'Error message should have included ' + tojson(objMsg));
});

tests.push(function assertShouldNotAcceptNonObjStringFunctionAsMsg() {
    const err = assert.throws(() => {
        assert(true, 1234);
    });

    assert.neq(-1, err.message.indexOf("msg parameter must be a "));
});

/* assert.eq tests */

tests.push(function eqShouldPassOnEquality() {
    assert.doesNotThrow(() => {
        assert.eq(3, 3);
    });
});

tests.push(function eqShouldFailWhenNotEqual() {
    assert.throws(() => {
        assert.eq(2, 3);
    });
});

tests.push(function eqShouldNotCallMsgFunctionOnSuccess() {
    var called = false;

    assert.doesNotThrow(() => {
        assert.eq(3, 3, () => {
            called = true;
        });
    });

    assert.eq(false, called, 'msg function should not have been called');
});

tests.push(function eqShouldCallMsgFunctionOnFailure() {
    var called = false;

    assert.throws(() => {
        assert.eq(1, 3, () => {
            called = true;
        });
    });

    assert.eq(true, called, 'msg function should have been called');
});

tests.push(function eqShouldPassOnObjectsWithSameContent() {
    const a = {'foo': true};
    const b = {'foo': true};

    assert.doesNotThrow(() => {
        assert.eq(a, b);
    }, [], 'eq should not throw exception on two objects with the same content');
});

/* assert.neq tests */

tests.push(function neqShouldFailOnEquality() {
    assert.throws(() => {
        assert.neq(3, 3);
    });
});

tests.push(function neqShouldPassWhenNotEqual() {
    assert.doesNotThrow(() => {
        assert.neq(2, 3);
    });
});

tests.push(function neqShouldFailOnObjectsWithSameContent() {
    const a = {'foo': true};
    const b = {'foo': true};

    assert.throws(() => {
        assert.neq(a, b);
    }, [], 'neq should throw exception on two objects with the same content');
});

/* assert.hasFields tests */

tests.push(function hasFieldsRequiresAnArrayOfFields() {
    const object = {field1: 1, field2: 1, field3: 1};

    assert.throws(() => {
        assert.hasFields(object, 'field1');
    });
});

tests.push(function hasFieldsShouldPassWhenObjectHasField() {
    const object = {field1: 1, field2: 1, field3: 1};

    assert.doesNotThrow(() => {
        assert.hasFields(object, ['field1']);
    });
});

tests.push(function hasFieldsShouldFailWhenObjectDoesNotHaveField() {
    const object = {field1: 1, field2: 1, field3: 1};

    assert.throws(() => {
        assert.hasFields(object, ['fieldDoesNotExist']);
    });
});

/* assert.contains tests */

tests.push(function containsShouldOnlyWorkOnArrays() {
    assert.throws(() => {
        assert.contains(42, 5);
    });
});

tests.push(function containsShouldPassIfArrayContainsValue() {
    const array = [1, 2, 3];

    assert.doesNotThrow(() => {
        assert.contains(2, array);
    });
});

tests.push(function containsShouldFailIfArrayDoesNotContainValue() {
    const array = [1, 2, 3];

    assert.throws(() => {
        assert.contains(42, array);
    });
});

/* assert.soon tests */

tests.push(function soonPassesWhenFunctionPasses() {
    assert.doesNotThrow(() => {
        assert.soon(() => {
            return true;
        });
    });
});

tests.push(function soonFailsIfMethodNeverPasses() {
    assert.throws(() => {
        assert.soon(() => {
            return false;
        }, 'assert message', kSmallTimeoutMS, kSmallRetryIntervalMS, {runHangAnalyzer: false});
    });
});

tests.push(function soonPassesIfMethodEventuallyPasses() {
    var count = 0;
    assert.doesNotThrow(() => {
        assert.soon(() => {
            count += 1;
            return count === 3;
        }, 'assert message', kDefaultTimeoutMS, kSmallRetryIntervalMS);
    });
});

/* assert.soonNoExcept tests */

tests.push(function soonNoExceptEventuallyPassesEvenWithExceptions() {
    var count = 0;
    assert.doesNotThrow(() => {
        assert.soonNoExcept(() => {
            count += 1;
            if (count < 3) {
                throw new Error('failed');
            }
            return true;
        }, 'assert message', kDefaultTimeoutMS, kSmallRetryIntervalMS);
    });
});

tests.push(function soonNoExceptFailsIfExceptionAlwaysThrown() {
    var count = 0;
    assert.throws(() => {
        assert.soonNoExcept(() => {
            throw new Error('failed');
        }, 'assert message', kSmallTimeoutMS, kSmallRetryIntervalMS, {runHangAnalyzer: false});
    });
});

/* assert.retry tests */

tests.push(function retryPassesAfterAFewAttempts() {
    var count = 0;

    assert.doesNotThrow(() => {
        assert.retry(() => {
            count += 1;
            return count === 3;
        }, 'assert message', kDefaultRetryAttempts, kSmallRetryIntervalMS);
    });
});

tests.push(function retryFailsAfterMaxAttempts() {
    assert.throws(() => {
        assert.retry(() => {
            return false;
        }, 'assert message', kDefaultRetryAttempts, kSmallRetryIntervalMS, {
            runHangAnalyzer: false
        });
    });
});

/* assert.retryNoExcept tests */

tests.push(function retryNoExceptPassesAfterAFewAttempts() {
    var count = 0;

    assert.doesNotThrow(() => {
        assert.retryNoExcept(() => {
            count += 1;
            if (count < 3) {
                throw new Error('failed');
            }
            return count === 3;
        }, 'assert message', kDefaultRetryAttempts, kSmallRetryIntervalMS);
    });
});

tests.push(function retryNoExceptFailsAfterMaxAttempts() {
    assert.throws(() => {
        assert.retryNoExcept(() => {
            throw new Error('failed');
        }, 'assert message', kDefaultRetryAttempts, kSmallRetryIntervalMS, {
            runHangAnalyzer: false
        });
    });
});

/* assert.time tests */

tests.push(function timeIsSuccessfulIfFuncExecutesInTime() {
    assert.doesNotThrow(() => {
        assert.time(() => {
            return true;
        }, 'assert message', kDefaultTimeoutMS);
    });
});

tests.push(function timeFailsIfFuncDoesNotFinishInTime() {
    assert.throws(() => {
        assert.time(() => {
            return true;
        }, 'assert message', -5 * 60 * 1000, {runHangAnalyzer: false});
    });
});

/* assert.isnull tests */

tests.push(function isnullPassesOnNull() {
    assert.doesNotThrow(() => {
        assert.isnull(null);
    });
});

tests.push(function isnullPassesOnUndefined() {
    assert.doesNotThrow(() => {
        assert.isnull(undefined);
    });
});

tests.push(function isnullFailsOnNotNull() {
    assert.throws(() => {
        assert.isnull('hello world');
    });
});

/* assert.lt tests */

tests.push(function ltPassesWhenLessThan() {
    assert.doesNotThrow(() => {
        assert.lt(3, 5);
    });
});

tests.push(function ltFailsWhenNotLessThan() {
    assert.throws(() => {
        assert.lt(5, 3);
    });
});

tests.push(function ltFailsWhenEqual() {
    assert.throws(() => {
        assert.lt(5, 5);
    });
});

tests.push(function ltPassesWhenLessThanWithTimestamps() {
    assert.doesNotThrow(() => {
        assert.lt(Timestamp(3, 0), Timestamp(10, 0));
    });
});

tests.push(function ltFailsWhenNotLessThanWithTimestamps() {
    assert.throws(() => {
        assert.lt(Timestamp(0, 10), Timestamp(0, 3));
    });
});

tests.push(function ltFailsWhenEqualWithTimestamps() {
    assert.throws(() => {
        assert.lt(Timestamp(5, 0), Timestamp(5, 0));
    });
});

/* assert.gt tests */

tests.push(function gtPassesWhenGreaterThan() {
    assert.doesNotThrow(() => {
        assert.gt(5, 3);
    });
});

tests.push(function gtFailsWhenNotGreaterThan() {
    assert.throws(() => {
        assert.gt(3, 5);
    });
});

tests.push(function gtFailsWhenEqual() {
    assert.throws(() => {
        assert.gt(5, 5);
    });
});

/* assert.lte tests */

tests.push(function ltePassesWhenLessThan() {
    assert.doesNotThrow(() => {
        assert.lte(3, 5);
    });
});

tests.push(function lteFailsWhenNotLessThan() {
    assert.throws(() => {
        assert.lte(5, 3);
    });
});

tests.push(function ltePassesWhenEqual() {
    assert.doesNotThrow(() => {
        assert.lte(5, 5);
    });
});

/* assert.gte tests */

tests.push(function gtePassesWhenGreaterThan() {
    assert.doesNotThrow(() => {
        assert.gte(5, 3);
    });
});

tests.push(function gteFailsWhenNotGreaterThan() {
    assert.throws(() => {
        assert.gte(3, 5);
    });
});

tests.push(function gtePassesWhenEqual() {
    assert.doesNotThrow(() => {
        assert.gte(5, 5);
    });
});

tests.push(function gtePassesWhenGreaterThanWithTimestamps() {
    assert.doesNotThrow(() => {
        assert.gte(Timestamp(0, 10), Timestamp(0, 3));
    });
});

tests.push(function gteFailsWhenNotGreaterThanWithTimestamps() {
    assert.throws(() => {
        assert.gte(Timestamp(0, 3), Timestamp(0, 10));
    });
});

tests.push(function gtePassesWhenEqualWIthTimestamps() {
    assert.doesNotThrow(() => {
        assert.gte(Timestamp(5, 0), Timestamp(5, 0));
    });
});

/* assert.betweenIn tests */

tests.push(function betweenInPassWhenNumberIsBetween() {
    assert.doesNotThrow(() => {
        assert.betweenIn(3, 4, 5);
    });
});

tests.push(function betweenInFailsWhenNumberIsNotBetween() {
    assert.throws(() => {
        assert.betweenIn(3, 5, 4);
    });
});

tests.push(function betweenInPassWhenNumbersEqual() {
    assert.doesNotThrow(() => {
        assert.betweenIn(3, 3, 5);
    });
    assert.doesNotThrow(() => {
        assert.betweenIn(3, 5, 5);
    });
});

/* assert.betweenEx tests */

tests.push(function betweenExPassWhenNumberIsBetween() {
    assert.doesNotThrow(() => {
        assert.betweenEx(3, 4, 5);
    });
});

tests.push(function betweenExFailsWhenNumberIsNotBetween() {
    assert.throws(() => {
        assert.betweenEx(3, 5, 4);
    });
});

tests.push(function betweenExFailsWhenNumbersEqual() {
    assert.throws(() => {
        assert.betweenEx(3, 3, 5);
    });
    assert.throws(() => {
        assert.betweenEx(3, 5, 5);
    });
});

/* assert.sameMembers tests */

tests.push(function sameMembersFailsWithInvalidArguments() {
    assert.throws(() => assert.sameMembers());
    assert.throws(() => assert.sameMembers([]));
    assert.throws(() => assert.sameMembers({}, {}));
    assert.throws(() => assert.sameMembers(1, 1));
});

tests.push(function sameMembersFailsWhenLengthsDifferent() {
    assert.throws(() => assert.sameMembers([], [1]));
    assert.throws(() => assert.sameMembers([], [1]));
    assert.throws(() => assert.sameMembers([1, 2], [1]));
    assert.throws(() => assert.sameMembers([1], [1, 2]));
});

tests.push(function sameMembersFailsWhenCountsOfDuplicatesDifferent() {
    assert.throws(() => assert.sameMembers([1, 1], [1, 2]));
    assert.throws(() => assert.sameMembers([1, 2], [1, 1]));
});

tests.push(function sameMembersFailsWithDifferentObjects() {
    assert.throws(() => assert.sameMembers([{_id: 0, a: 0}], [{_id: 0, a: 1}]));
    assert.throws(() => assert.sameMembers([{_id: 1, a: 0}], [{_id: 0, a: 0}]));
    assert.throws(() => {
        assert.sameMembers([{a: [{b: 0, c: 0}], _id: 0}], [{_id: 0, a: [{c: 0, b: 1}]}]);
    });
});

tests.push(function sameMembersFailsWithDifferentBSONTypes() {
    assert.throws(() => {
        assert.sameMembers([new BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq")],
                           [new BinData(0, "xxxgqwetkqwklEWRbWERKKJREtbq")]);
    });
    assert.throws(() => assert.sameMembers([new Timestamp(0, 1)], [new Timestamp(0, 2)]));
});

tests.push(function sameMembersFailsWithCustomCompareFn() {
    const compareBinaryEqual = (a, b) => bsonBinaryEqual(a, b);
    assert.throws(() => {
        assert.sameMembers([NumberLong(1)], [1], undefined /*msg*/, compareBinaryEqual);
    });
    assert.throws(() => {
        assert.sameMembers([NumberLong(1), NumberInt(2)],
                           [2, NumberLong(1)],
                           undefined /*msg*/,
                           compareBinaryEqual);
    });
});

tests.push(function sameMembersDoesNotSortNestedArrays() {
    assert.throws(() => assert.sameMembers([[1, 2]], [[2, 1]]));
    assert.throws(() => {
        assert.sameMembers([{a: [{b: 0}, {b: 1, c: 0}], _id: 0}],
                           [{_id: 0, a: [{c: 0, b: 1}, {b: 0}]}]);
    });
});

tests.push(function sameMembersPassesWithEmptyArrays() {
    assert.sameMembers([], []);
});

tests.push(function sameMembersPassesSingleElement() {
    assert.sameMembers([1], [1]);
});

tests.push(function sameMembersPassesWithSameOrder() {
    assert.sameMembers([1, 2], [1, 2]);
    assert.sameMembers([1, 2, 3], [1, 2, 3]);
});

tests.push(function sameMembersPassesWithDifferentOrder() {
    assert.sameMembers([2, 1], [1, 2]);
    assert.sameMembers([1, 2, 3], [3, 1, 2]);
});

tests.push(function sameMembersPassesWithDuplicates() {
    assert.sameMembers([1, 1, 2], [1, 1, 2]);
    assert.sameMembers([1, 1, 2], [1, 2, 1]);
    assert.sameMembers([2, 1, 1], [1, 1, 2]);
});

tests.push(function sameMembersPassesWithSortedNestedArrays() {
    assert.sameMembers([[1, 2]], [[1, 2]]);
    assert.sameMembers([{a: [{b: 0}, {b: 1, c: 0}], _id: 0}],
                       [{_id: 0, a: [{b: 0}, {c: 0, b: 1}]}]);
});

tests.push(function sameMembersPassesWithObjects() {
    assert.sameMembers([{_id: 0, a: 0}], [{_id: 0, a: 0}]);
    assert.sameMembers([{_id: 0, a: 0}, {_id: 1}], [{_id: 0, a: 0}, {_id: 1}]);
    assert.sameMembers([{_id: 0, a: 0}, {_id: 1}], [{_id: 1}, {_id: 0, a: 0}]);
});

tests.push(function sameMembersPassesWithUnsortedObjects() {
    assert.sameMembers([{a: 0, _id: 1}], [{_id: 1, a: 0}]);
    assert.sameMembers([{a: [{b: 1, c: 0}], _id: 0}], [{_id: 0, a: [{c: 0, b: 1}]}]);
});

tests.push(function sameMembersPassesWithBSONTypes() {
    assert.sameMembers([new BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq")],
                       [new BinData(0, "JANgqwetkqwklEWRbWERKKJREtbq")]);
    assert.sameMembers([new Timestamp(0, 1)], [new Timestamp(0, 1)]);
});

tests.push(function sameMembersPassesWithOtherTypes() {
    assert.sameMembers([null], [null]);
    assert.sameMembers([undefined], [undefined]);
    assert.sameMembers(["a"], ["a"]);
    assert.sameMembers([null, undefined, "a"], [undefined, "a", null]);
});

tests.push(function sameMembersDefaultCompareIsFriendly() {
    assert.sameMembers([NumberLong(1), NumberInt(2)], [2, 1]);
});

tests.push(function sameMembersPassesWithCustomCompareFn() {
    const compareBinaryEqual = (a, b) => bsonBinaryEqual(a, b);
    assert.sameMembers([[1, 2]], [[1, 2]], undefined /*msg*/, compareBinaryEqual);
    assert.sameMembers([NumberLong(1), NumberInt(2)],
                       [NumberInt(2), NumberLong(1)],
                       undefined /*msg*/,
                       compareBinaryEqual);
});

tests.push(function assertCallsHangAnalyzer() {
    function runAssertTest(f) {
        const oldMongoRunner = MongoRunner;
        let runs = 0;
        try {
            MongoRunner.runHangAnalyzer = function() {
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
    runAssertTest(
        () => assert.soon(() => false, 'assert message', kSmallTimeoutMS, kSmallRetryIntervalMS));
    runAssertTest(() => assert.retry(
                      () => false, 'assert message', kDefaultRetryAttempts, kSmallRetryIntervalMS));
    runAssertTest(() => assert.time(
                      () => sleep(5), 'assert message', 1 /* we certainly take less than this */));
});

tests.push(function serializeAssertionErrorWithoutAttributes() {
    let mongoAE = new AssertionError("Lorem ipsum", 9928000);
    // Remove the non-deterministic timestamp.
    delete mongoAE._fixedProperties.t;
    const expectedObj =
        {s: "E", c: "js_test", id: 9928000, ctx: "shell_assertions", msg: "Lorem ipsum"};
    assert.eq(JSON.stringify(expectedObj), mongoAE.toString());
    assert.eq(tojson(expectedObj, "", true), mongoAE.tojson("", true));
});

tests.push(function serializeAssertionErrorWithAttributes() {
    let mongoAE = new AssertionError("Oops!", 9928000, {hello: "world"});
    // Remove the non-deterministic timestamp.
    delete mongoAE._fixedProperties.t;
    const expectedObj = {
        s: "E",
        c: "js_test",
        id: 9928000,
        ctx: "shell_assertions",
        msg: "Oops!",
        attr: {hello: "world"}
    };
    assert.eq(JSON.stringify(expectedObj), mongoAE.toString());
    assert.eq(tojson(expectedObj, "", true), mongoAE.tojson("", true));
});

function assertThrowsAssertionErrorWithJson(assertFailureTriggerFn, expectedJsonFields) {
    const oldLogFormat = TestData.logFormat;
    try {
        TestData.logFormat = "json";
        assertFailureTriggerFn();
    } catch (e) {
        const {t, ...jsonWithoutTimestamp} =
            assert.doesNotThrow(JSON.parse,
                                [e.toString()],
                                "could not parse assertion error string",
                                {error: e.toString()});
        assert.docEq(
            {s: "E", c: "js_test", id: 9928000, ctx: "shell_assertions", ...expectedJsonFields},
            jsonWithoutTimestamp,
            "unexpected assertion error");
    } finally {
        TestData.logFormat = oldLogFormat;
    }
    // Call the 'assertFailureTriggerFn' second time to make sure it actually throws.
    assert.throws(assertFailureTriggerFn, [], "assertFailureTriggerFn");
}

tests.push(function assertJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert(false, "lorem ipsum");
    }, {msg: "assert failed : lorem ipsum"});
    assertThrowsAssertionErrorWithJson(() => {
        assert(false, "lorem ipsum", kAttr);
    }, {msg: "assert failed : lorem ipsum", attr: {...kAttr}});
});

tests.push(function assertEqJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.eq(5, 2 + 2, "lorem ipsum");
    }, {msg: "assert.eq() failed : lorem ipsum", attr: {a: 5, b: 4}});
    assertThrowsAssertionErrorWithJson(() => {
        assert.eq(5, 2 + 2, "lorem ipsum", kAttr);
    }, {msg: "assert.eq() failed : lorem ipsum", attr: {a: 5, b: 4, ...kAttr}});
});

tests.push(function assertDocEqJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.docEq({msg: "hello"}, {msg: "goodbye"}, "lorem ipsum", kAttr);
    }, {
        msg: "assert.docEq() failed : lorem ipsum",
        attr: {expectedDoc: {msg: "hello"}, actualDoc: {msg: "goodbye"}, ...kAttr}
    });
});

tests.push(function assertSetEqJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.setEq(new Set([1, 2, 3]), new Set([4, 5]), "lorem ipsum", kAttr);
    }, {
        msg: "assert.setEq() failed : lorem ipsum",
        attr: {expectedSet: [1, 2, 3], actualSet: [4, 5], ...kAttr}
    });
});

tests.push(function assertSameMembersJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.sameMembers([1, 2], [1], "Oops!", assert._isDocEq, kAttr);
    }, {
        msg: "assert.sameMembers() failed : Oops!",
        attr: {aArr: [1, 2], bArr: [1], compareFn: "_isDocEq", ...kAttr}
    });
});

tests.push(function assertFuzzySameMembersJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.fuzzySameMembers([{soccer: 42}], [{score: 42000}], ["score"], "Oops!", 4, kAttr);
    }, {
        msg: "assert.sameMembers() failed : Oops!",
        attr: {aArr: [{soccer: 42}], bArr: [{score: 42000}], compareFn: "fuzzyCompare", ...kAttr}
    });
});

tests.push(function assertNeqJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.neq(42, 42, "Oops!", kAttr);
    }, {msg: "assert.neq() failed : Oops!", attr: {a: 42, b: 42, ...kAttr}});
});

tests.push(function assertHasFieldsJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.hasFields({hello: "world"}, ["goodbye"], "Oops!", kAttr);
    }, {
        msg: "assert.hasFields() failed : Oops!",
        attr: {result: {hello: "world"}, arr: ["goodbye"], ...kAttr}
    });
});

tests.push(function assertContainsJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.contains(3, [14, 15, 926], "Oops!", kAttr);
    }, {msg: "assert.contains() failed : Oops!", attr: {o: 3, arr: [14, 15, 926], ...kAttr}});
});

tests.push(function assertDoesNotContainJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.doesNotContain(3, [3, 23], "Oops!", kAttr);
    }, {msg: "assert.doesNotContain() failed : Oops!", attr: {o: 3, arr: [3, 23], ...kAttr}});
});

tests.push(function assertContainsPrefixJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.containsPrefix("hello", ["hell", "help"], "Oops!", kAttr);
    }, {
        msg: "assert.containsPrefix() failed : Oops!",
        attr: {prefix: "hello", arr: ["hell", "help"], ...kAttr}
    });
});

tests.push(function assertSoonJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.soon(() => false, "Oops!", 20, 10, {runHangAnalyzer: false}, kAttr);
    }, {msg: "assert.soon failed (timeout 20ms), msg : Oops!", attr: {...kAttr}});
});

tests.push(function assertSoonNoExceptJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.soonNoExcept(() => {
            throw Error("disaster");
        }, "Oops!", 20, 10, {runHangAnalyzer: false}, kAttr);
    }, {msg: "assert.soon failed (timeout 20ms), msg : Oops!", attr: {...kAttr}});
});

tests.push(function assertRetryJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.retry(() => false, "Oops!", 2, 10, {runHangAnalyzer: false}, kAttr);
    }, {msg: "assert.retry() failed : Oops!", attr: {...kAttr}});
});

tests.push(function assertRetryNoExceptJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.retryNoExcept(() => {
            throw Error("disaster");
        }, "Oops!", 2, 10, {runHangAnalyzer: false}, kAttr);
    }, {msg: "assert.retry() failed : Oops!", attr: {...kAttr}});
});

tests.push(function assertTimeJsonFormat() {
    const sleepTimeMS = 20, timeoutMS = 10;
    const f = (() => {
        sleep(sleepTimeMS);
    });
    assertThrowsAssertionErrorWithJson(() => {
        try {
            assert.time(f, "Oops!", timeoutMS, {runHangAnalyzer: false}, kAttr);
        } catch (e) {
            // Override the 'timeMS' to make the test deterministic.
            e.attr.timeMS = sleepTimeMS;
            throw e;
        }
    }, {msg: "assert.time() failed : Oops!", attr: {timeMS: sleepTimeMS, timeoutMS, ...kAttr}});
});

tests.push(function assertThrowsJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.throws(() => true, [], "Oops!", kAttr);
    }, {msg: "did not throw exception : Oops!", attr: {...kAttr}});
});

tests.push(function assertThrowsWithCodeJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        const err = new Error("disaster");
        err.code = 24;
        assert.throwsWithCode(() => {
            throw err;
        }, 42, [], "Oops!", kAttr);
    }, {
        msg: "assert.throwsWithCode() failed : Oops!",
        attr: {code: 24, expectedCode: [42], ...kAttr}
    });
});

tests.push(function assertDoesNotThrowJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        const err = new Error("disaster");
        assert.doesNotThrow(() => {
            throw err;
        }, [], "Oops!", kAttr);
    }, {
        msg: "assert.doesNotThrow() failed : Oops!",
        attr: {error: {message: "disaster"}, ...kAttr}
    });
});

tests.push(function assertCommandWorkedWrongArgumentTypeJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.commandWorked("cmd", "Oops!");
    }, {
        msg: "expected result type 'object'" +
            " : unexpected result type given to assert.commandWorked()",
        attr: {result: "cmd", resultType: "string"}
    });
});

tests.push(function assertCommandWorkedJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        const res = {
            ok: 0,
            code: ErrorCodes.BadValue,
            codeName: ErrorCodeStrings[ErrorCodes.BadValue],
            errmsg: "unexpected error",
            _mongo: "connection to localhost:20000",
            _commandObj: {hello: 1}
        };
        assert.commandWorked(res, "Oops!");
    }, {
        msg: "command failed : Oops!",
        attr: {
            res: {
                ok: 0,
                code: ErrorCodes.BadValue,
                codeName: ErrorCodeStrings[ErrorCodes.BadValue],
                errmsg: "unexpected error",
                _mongo: "connection to localhost:20000",  // Will not be seen when 'res' is BSON.
                _commandObj: {hello: 1}                   // Will not be seen when 'res' is BSON.
            },
            originalCommand: {hello: 1},
            connection: "connection to localhost:20000"
        }
    });
});

tests.push(function assertCommandFailedJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        const res = {ok: 1, _mongo: "connection to localhost:20000", _commandObj: {hello: 1}};
        assert.commandFailed(res, "Oops!");
    }, {
        msg: "command worked when it should have failed : Oops!",
        attr: {
            res: {
                ok: 1,
                _mongo: "connection to localhost:20000",  // Will not be seen when 'res' is BSON.
                _commandObj: {hello: 1}                   // Will not be seen when 'res' is BSON.
            },
            originalCommand: {hello: 1},
            connection: "connection to localhost:20000"
        }
    });
});

tests.push(function assertCommandFailedWithCodeJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        const res = {ok: 1, _mongo: "connection to localhost:20000", _commandObj: {hello: 1}};
        assert.commandFailedWithCode(res, ErrorCodes.BadValue, "Oops!");
    }, {
        msg: "command worked when it should have failed : Oops!",
        attr: {
            res: {
                ok: 1,
                _mongo: "connection to localhost:20000",  // Will not be seen when 'res' is BSON.
                _commandObj: {hello: 1}                   // Will not be seen when 'res' is BSON.
            },
            originalCommand: {hello: 1},
            connection: "connection to localhost:20000"
        }
    });
});

tests.push(function assertCommandFailedWithWrongCodeJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        const res = {
            ok: 0,
            code: ErrorCodes.BadValue,
            codeName: ErrorCodeStrings[ErrorCodes.BadValue],
            errmsg: "unexpected error",
            _mongo: "connection to localhost:20000",
            _commandObj: {hello: 1}
        };
        assert.commandFailedWithCode(res, ErrorCodes.NetworkTimeout, "Oops!");
    }, {
        msg: "command did not fail with any of the following codes : Oops!",
        attr: {
            res: {
                ok: 0,
                code: ErrorCodes.BadValue,
                codeName: ErrorCodeStrings[ErrorCodes.BadValue],
                errmsg: "unexpected error",
                _mongo: "connection to localhost:20000",  // Will not be seen when 'res' is BSON.
                _commandObj: {hello: 1}                   // Will not be seen when 'res' is BSON.
            },
            expectedCode: [89],
            originalCommand: {hello: 1},
            connection: "connection to localhost:20000"
        }
    });
});

tests.push(function assertWriteOKJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        const res = {ok: 0};
        assert.writeOK(res, "Oops!");
    }, {msg: "unknown type of write result, cannot check ok : Oops!", attr: {res: {ok: 0}}});
});

tests.push(function assertWriteErrorJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        const res =
            new WriteResult({nRemoved: 0, writeErrors: [], upserted: []}, 3, {w: "majority"});
        assert.writeError(res, "Oops!");
    }, {msg: "no write error : Oops!", attr: {res: {nRemoved: 0}}});
});

tests.push(function assertWriteErrorWithCodeJsonFormat() {
    const writeError = {code: ErrorCodes.NetworkTimeout, errmsg: "Timeout!"};
    assertThrowsAssertionErrorWithJson(() => {
        const res = new WriteResult(
            {nRemoved: 0, writeErrors: [writeError], upserted: []}, 3, {w: "majority"});
        assert.writeErrorWithCode(res, ErrorCodes.BadValue, "Oops!");
    }, {
        msg: "found code(s) does not match any of the expected codes : Oops!",
        attr: {
            res: {nRemoved: 0},
            expectedCode: [ErrorCodes.BadValue],
            writeErrorCodes: [ErrorCodes.NetworkTimeout]
        }
    });
});

tests.push(function assertIsNullJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.isnull({ok: 1}, "Oops!", kAttr);
    }, {msg: "assert.isnull() failed : Oops!", attr: {what: {ok: 1}, ...kAttr}});
});

tests.push(function assertLTJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.lt(41, 18, "Oops!", kAttr);
    }, {msg: "assert less than failed : Oops!", attr: {a: 41, b: 18, ...kAttr}});
});

tests.push(function assertBetweenJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.between(1, 15, 10, "Oops!", true, kAttr);
    }, {
        msg: "assert.between() failed : Oops!",
        attr: {a: 1, b: 15, c: 10, inclusive: true, ...kAttr}
    });
});

tests.push(function assertCloseJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.close(123.4567, 123.4678, "Oops!");
    }, {msg: "assert.close() failed : Oops!", attr: {a: 123.4567, b: 123.4678, places: 4}});
});

tests.push(function assertCloseWithinMSJsonFormat() {
    const dateForLog = (arg) => JSON.parse(JSON.stringify(arg));
    const date1 = new Date();
    sleep(10);
    const date2 = new Date();
    assertThrowsAssertionErrorWithJson(() => {
        assert.closeWithinMS(date1, date2, "Oops!", 1, kAttr);
    }, {
        msg: "assert.closeWithinMS() failed : Oops!",
        attr: {a: dateForLog(date1), b: dateForLog(date2), deltaMS: 1, ...kAttr}
    });
});

tests.push(function assertIncludesJsonFormat() {
    assertThrowsAssertionErrorWithJson(() => {
        assert.includes("farmacy", "ace", "Oops!", kAttr);
    }, {
        msg: "assert.includes() failed : Oops!",
        attr: {haystack: "farmacy", needle: "ace", ...kAttr}
    });
});

const jsTestLogUtils = {
    setup: (test, logLevel = 4) => {
        const oldTestData = TestData;
        const printOriginal = print;
        // In case an exception is thrown, revert the original print and TestData states.
        try {
            // Override the TestData object.
            TestData = {...TestData, logFormat: "json", logLevel};
            // Override the print function.
            print = msg => {
                print.console.push(msg);
            };
            print.console = [];
            test();
        } finally {
            // Reset state.
            print = printOriginal;
            TestData = oldTestData;
        }
    },
    getCapturedJSONOutput: (loggingFn, assertPrinted = true) => {
        loggingFn();
        // Assert we only print once.
        if (assertPrinted)
            assert.eq(1, print.console.length);
        let printedJson = print.console[0] ? JSON.parse(print.console[0]) : {};
        delete printedJson["t"];
        // Reset the console
        print.console = [];
        return printedJson;
    }
};

tests.push(function assertJsTestLogJsonFormat() {
    const extraArgs = {attr: {hello: "world", foo: "bar"}, id: 87};
    jsTestLogUtils.setup(() => {
        const printedJson =
            jsTestLogUtils.getCapturedJSONOutput(() => jsTestLog("test message", extraArgs));
        const expectedJson = {
            "s": "I",
            "c": "js_test",
            "ctx": "shell_assertions",
            "msg": "test message",
            "attr": extraArgs.attr,
            "id": extraArgs.id,
        };
        assert.docEq(expectedJson, printedJson, "expected a different log format");

        // Assert the legacy format works as before.
        TestData.logFormat = "legacy";
        jsTestLog("test message legacy", extraArgs);
        assert.eq(1, print.console.length);
        const expectedLegacyResult =
            ["----", "test message legacy", "----"].map(s => `[jsTest] ${s}`);
        assert.eq(`\n\n${expectedLegacyResult.join("\n")}\n\n`,
                  print.console,
                  "expected a different log format when legacy mode is on");
    });
});

tests.push(function assertLogSeverities() {
    const severities = {"I": "info", "D": "debug", "W": "warning", "E": "error"};
    const extraArgs = {attr: {hello: "world", foo: "bar"}, id: 87};
    jsTestLogUtils.setup(() => {
        for (const [severity, logFnName] of Object.entries(severities)) {
            const printedJson = jsTestLogUtils.getCapturedJSONOutput(
                () => jsTest.log[logFnName]("test message", extraArgs));
            const expectedJson = {
                "s": severity,
                "c": "js_test",
                "ctx": "shell_assertions",
                "msg": "test message",
                "attr": extraArgs.attr,
                "id": extraArgs.id,
            };
            assert.docEq(expectedJson, printedJson, "expected a different log to be printed");
        }
        // Assert default logging uses the info severity and that extra arguments are ignored.
        const testMsg = "info is default.";
        const printedLogInfo = jsTestLogUtils.getCapturedJSONOutput(
            () => jsTest.log(testMsg, {...extraArgs, nonUsefulProp: "some value"}));
        const printedLogDefault =
            jsTestLogUtils.getCapturedJSONOutput(() => jsTest.log.info(testMsg, extraArgs));
        assert.docEq(printedLogInfo, printedLogDefault, "Expected default log severity to be info");
    });
});

tests.push(function assertLogsAreFilteredBySeverity() {
    const extraArgs = {attr: {hello: "world", foo: "bar"}, id: 87};
    const severityFunctionNames = ["info", "debug", "warning", "error"];
    // For each possible log level, test that all logs with greater severity are skipped.
    [1, 2, 3, 4].forEach(logLevel => {
        jsTestLogUtils.setup(() => {
            const printedLogs =
                severityFunctionNames
                    .map(logFnName => {
                        return jsTestLogUtils.getCapturedJSONOutput(
                            () => jsTest.log[logFnName]("test message", extraArgs), false);
                    })
                    .filter(log => Object.keys(log).length > 0);
            assert.eq(
                logLevel, printedLogs.length, "Printed a different number of logs: " + logLevel);
        }, logLevel);
    });
});

tests.push(function assertInvalidLogLevelAndSeverityThrows() {
    [0, 5, "a", {a: 4}, {}].forEach(invalidLogLevel => {
        jsTestLogUtils.setup(() => {
            assert.throws(() => jsTest.log.info("This should throw."),
                          [],
                          "Invalid log levels should throw an exception.");
        }, invalidLogLevel);
    });
    ["q", 0, 12, {hello: "world"}, {}].forEach(invalidSeverity => {
        jsTestLogUtils.setup(() => {
            assert.throws(() => jsTest.log("This should throw.", {severity: invalidSeverity}),
                          [],
                          "Invalid severity should throw an exception.");
        }, 4);
    });
});

/* main */

tests.forEach((test) => {
    jsTest.log(`Starting tests '${test.name}'`);
    test();
});
