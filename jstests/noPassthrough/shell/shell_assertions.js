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
    let called = false;

    assert(true, () => {
        called = true;
    });

    assert.eq(false, called, 'called should not have been udpated');
});

tests.push(function assertShouldCallMsgFunctionsOnFailure() {
    let called = false;

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
    let called = false;

    assert.doesNotThrow(() => {
        assert.eq(3, 3, () => {
            called = true;
        });
    });

    assert.eq(false, called, 'msg function should not have been called');
});

tests.push(function eqShouldCallMsgFunctionOnFailure() {
    let called = false;

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
    let count = 0;
    assert.doesNotThrow(() => {
        assert.soon(() => {
            count += 1;
            return count === 3;
        }, 'assert message', kDefaultTimeoutMS, kSmallRetryIntervalMS);
    });
});

/* assert.soonNoExcept tests */

tests.push(function soonNoExceptEventuallyPassesEvenWithExceptions() {
    let count = 0;
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
    let count = 0;
    assert.throws(() => {
        assert.soonNoExcept(() => {
            throw new Error('failed');
        }, 'assert message', kSmallTimeoutMS, kSmallRetryIntervalMS, {runHangAnalyzer: false});
    });
});

/* assert.retry tests */

tests.push(function retryPassesAfterAFewAttempts() {
    let count = 0;

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
    let count = 0;

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

tests.push(function assertJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert(false, "lorem ipsum");
    }, {msg: "assert failed : lorem ipsum"});
    assertThrowsErrorWithJson(() => {
        assert(false, "lorem ipsum", kAttr);
    }, {msg: "assert failed : lorem ipsum", attr: {...kAttr}});
});

tests.push(function assertEqJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.eq(5, 2 + 2, "lorem ipsum");
    }, {msg: "[{a}] and [{b}] are not equal : lorem ipsum", attr: {a: 5, b: 4}});
    assertThrowsErrorWithJson(() => {
        assert.eq(5, 2 + 2, "lorem ipsum", kAttr);
    }, {msg: "[{a}] and [{b}] are not equal : lorem ipsum", attr: {a: 5, b: 4, ...kAttr}});
});

tests.push(function assertDocEqJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.docEq({msg: "hello"}, {msg: "goodbye"}, "lorem ipsum", kAttr);
    }, {
        msg:
            "expected document {expectedDoc} and actual document {actualDoc} are not equal : lorem ipsum",
        attr: {expectedDoc: {msg: "hello"}, actualDoc: {msg: "goodbye"}, ...kAttr}
    });
});

tests.push(function assertSetEqJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.setEq(new Set([1, 2, 3]), new Set([4, 5]), "lorem ipsum", kAttr);
    }, {
        msg: "expected set {expectedSet} and actual set {actualSet} are not equal : lorem ipsum",
        attr: {expectedSet: [1, 2, 3], actualSet: [4, 5], ...kAttr}
    });
});

tests.push(function assertSameMembersJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.sameMembers([1, 2], [1], "Oops!", assert._isDocEq, kAttr);
    }, {
        msg: "{aArr} != {bArr} : Oops!",
        attr: {aArr: [1, 2], bArr: [1], compareFn: "_isDocEq", ...kAttr}
    });
});

tests.push(function assertFuzzySameMembersJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.fuzzySameMembers([{soccer: 42}], [{score: 42000}], ["score"], "Oops!", 4, kAttr);
    }, {
        msg: "{aArr} != {bArr} : Oops!",
        attr: {aArr: [{soccer: 42}], bArr: [{score: 42000}], compareFn: "fuzzyCompare", ...kAttr}
    });
});

tests.push(function assertNeqJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.neq(42, 42, "Oops!", kAttr);
    }, {msg: "[{a}] and [{b}] are equal : Oops!", attr: {a: 42, b: 42, ...kAttr}});
});

tests.push(function assertHasFieldsJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.hasFields({hello: "world"}, ["goodbye"], "Oops!", kAttr);
    }, {
        msg: "Not all of the values from {arr} were in {obj} : Oops!",
        attr: {obj: {hello: "world"}, arr: ["goodbye"], ...kAttr}
    });
});

tests.push(function assertContainsJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.contains(3, [14, 15, 926], "Oops!", kAttr);
    }, {
        msg: "{element} was not in {arr} : Oops!",
        attr: {element: 3, arr: [14, 15, 926], ...kAttr}
    });
});

tests.push(function assertDoesNotContainJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.doesNotContain(3, [3, 23], "Oops!", kAttr);
    }, {msg: "{element} is in {arr} : Oops!", attr: {element: 3, arr: [3, 23], ...kAttr}});
});

tests.push(function assertContainsPrefixJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.containsPrefix("hello", ["hell", "help"], "Oops!", kAttr);
    }, {
        msg: "{prefix} was not a prefix in {arr} : Oops!",
        attr: {prefix: "hello", arr: ["hell", "help"], ...kAttr}
    });
});

tests.push(function assertSoonJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.soon(() => false, "Oops!", 20, 10, {runHangAnalyzer: false}, kAttr);
    }, {msg: "assert.soon failed (timeout 20ms), msg : Oops!", attr: {...kAttr}});
});

tests.push(function assertSoonNoExceptJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.soonNoExcept(() => {
            throw Error("disaster");
        }, "Oops!", 20, 10, {runHangAnalyzer: false}, kAttr);
    }, {msg: "assert.soon failed (timeout 20ms), msg : Oops!", attr: {...kAttr}});
});

tests.push(function assertRetryJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.retry(() => false, "Oops!", 2, 10, {runHangAnalyzer: false}, kAttr);
    }, {msg: "Oops!", attr: {...kAttr}});
});

tests.push(function assertRetryNoExceptJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.retryNoExcept(() => {
            throw Error("disaster");
        }, "Oops!", 2, 10, {runHangAnalyzer: false}, kAttr);
    }, {msg: "Oops!", attr: {...kAttr}});
});

tests.push(function assertTimeJsonFormat() {
    const sleepTimeMS = 20, timeoutMS = 10;
    const f = (() => {
        sleep(sleepTimeMS);
    });
    assertThrowsErrorWithJson(() => {
        try {
            assert.time(f, "Oops!", timeoutMS, {runHangAnalyzer: false}, kAttr);
        } catch (e) {
            // Override 'timeMS' to make the test deterministic.
            e.extraAttr.timeMS = sleepTimeMS;
            // Override 'diff' to make the test deterministic.
            e.extraAttr.diff = sleepTimeMS;
            throw e;
        }
    }, {
        msg: "assert.time failed : Oops!",
        attr: {timeMS: sleepTimeMS, timeoutMS, function: f, diff: sleepTimeMS, ...kAttr}
    });
});

tests.push(function assertThrowsJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.throws(() => true, [], "Oops!", kAttr);
    }, {msg: "did not throw exception : Oops!", attr: {...kAttr}});
});

tests.push(function assertThrowsWithCodeJsonFormat() {
    assertThrowsErrorWithJson(() => {
        const err = new Error("disaster");
        err.code = 24;
        assert.throwsWithCode(() => {
            throw err;
        }, 42, [], "Oops!", kAttr);
    }, {
        msg: "[{code}] and [{expectedCode}] are not equal : Oops!",
        attr: {code: 24, expectedCode: [42], ...kAttr}
    });
});

tests.push(function assertDoesNotThrowJsonFormat() {
    assertThrowsErrorWithJson(() => {
        const err = new Error("disaster");
        assert.doesNotThrow(() => {
            throw err;
        }, [], "Oops!", kAttr);
    }, {
        msg: "threw unexpected exception: {error} : Oops!",
        attr: {error: {message: "disaster"}, ...kAttr}
    });
});

tests.push(function assertCommandWorkedWrongArgumentTypeJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.commandWorked("cmd", "Oops!");
    }, {
        msg:
            "expected result type 'object', got '{resultType}', res='{result}' : unexpected result type given to assert.commandWorked()",
        attr: {result: "cmd", resultType: "string"}
    });
});

tests.push(function assertCommandWorkedJsonFormat() {
    assertThrowsErrorWithJson(() => {
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
        msg:
            "command failed: {res} with original command request: {originalCommand} with errmsg: unexpected error : Oops!",
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
    assertThrowsErrorWithJson(() => {
        const res = {ok: 1, _mongo: "connection to localhost:20000", _commandObj: {hello: 1}};
        assert.commandFailed(res, "Oops!");
    }, {
        msg: "command worked when it should have failed: {res} : Oops!",
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
    assertThrowsErrorWithJson(() => {
        const res = {ok: 1, _mongo: "connection to localhost:20000", _commandObj: {hello: 1}};
        assert.commandFailedWithCode(res, ErrorCodes.BadValue, "Oops!");
    }, {
        msg: "command worked when it should have failed: {res} : Oops!",
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
    assertThrowsErrorWithJson(() => {
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
        msg:
            "command did not fail with any of the following codes {expectedCode} {res}. errmsg: unexpected error : Oops!",
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
    assertThrowsErrorWithJson(() => {
        const res = {ok: 0};
        assert.writeOK(res, "Oops!");
    }, {msg: "unknown type of write result, cannot check ok: {res} : Oops!", attr: {res: {ok: 0}}});
});

tests.push(function assertWriteErrorJsonFormat() {
    assertThrowsErrorWithJson(() => {
        const res =
            new WriteResult({nRemoved: 0, writeErrors: [], upserted: []}, 3, {w: "majority"});
        assert.writeError(res, "Oops!");
    }, {
        msg: "no write error: {res} : Oops!",
        attr: {
            res: {
                ok: {"$undefined": true},
                nInserted: {"$undefined": true},
                nUpserted: {"$undefined": true},
                nMatched: {"$undefined": true},
                nModified: {"$undefined": true},
                nRemoved: 0
            }
        }
    });
});

tests.push(function assertWriteErrorWithCodeJsonFormat() {
    const writeError = {code: ErrorCodes.NetworkTimeout, errmsg: "Timeout!"};
    assertThrowsErrorWithJson(() => {
        const res = new WriteResult(
            {nRemoved: 0, writeErrors: [writeError], upserted: []}, 3, {w: "majority"});
        assert.writeErrorWithCode(res, ErrorCodes.BadValue, "Oops!");
    }, {
        msg:
            "found code(s) {writeErrorCodes} does not match any of the expected codes {expectedCode}. Original command response: {res} : Oops!",
        attr: {
            res: {
                ok: {"$undefined": true},
                nInserted: {"$undefined": true},
                nUpserted: {"$undefined": true},
                nMatched: {"$undefined": true},
                nModified: {"$undefined": true},
                nRemoved: 0
            },
            expectedCode: [ErrorCodes.BadValue],
            writeErrorCodes: [ErrorCodes.NetworkTimeout]
        }
    });
});

tests.push(function assertIsNullJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.isnull({ok: 1}, "Oops!", kAttr);
    }, {msg: "supposed to be null, was: {value} : Oops!", attr: {value: {ok: 1}, ...kAttr}});
});

tests.push(function assertLTJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.lt(41, 18, "Oops!", kAttr);
    }, {msg: "{a} is not less than {b} : Oops!", attr: {a: 41, b: 18, ...kAttr}});
});

tests.push(function assertBetweenJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.between(1, 15, 10, "Oops!", true, kAttr);
    }, {
        msg: "{b} is not between {a} and {c} : Oops!",
        attr: {a: 1, b: 15, c: 10, inclusive: true, ...kAttr}
    });
});

tests.push(function assertCloseJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.close(123.4567, 123.4678, "Oops!");
    }, {
        msg:
            "123.4567 is not equal to 123.4678 within 4 places, absolute error: 0.011099999999999, relative error: 0.00008990198254118888 : Oops!"
    });
});

tests.push(function assertCloseWithinMSJsonFormat() {
    const dateForLog = (arg) => JSON.parse(JSON.stringify(arg));
    const date1 = Date.UTC(1970, 0, 1, 23, 59, 59, 999);
    const date2 = date1 + 10;
    assertThrowsErrorWithJson(() => {
        assert.closeWithinMS(date1, date2, "Oops!", 1, kAttr);
    }, {
        msg: "86399999 is not equal to 86400009 within 1 millis, actual delta: 10 millis : Oops!",
        attr: {a: dateForLog(date1), b: dateForLog(date2), deltaMS: 1, ...kAttr}
    });
});

tests.push(function assertIncludesJsonFormat() {
    assertThrowsErrorWithJson(() => {
        assert.includes("farmacy", "ace", "Oops!", kAttr);
    }, {
        msg: "string [{haystack}] does not include [{needle}] : Oops!",
        attr: {haystack: "farmacy", needle: "ace", ...kAttr}
    });
});

tests.push(function assertIgnoreNonObjectExtraAttr() {
    const err = new Error("Oops!");
    err.extraAttr = "not an object";
    err.code = ErrorCodes.JSInterpreterFailure;
    assert.throwsWithCode(() => {
        throw err;
    }, ErrorCodes.JSInterpreterFailure);
});

/* main */

tests.forEach((test) => {
    jsTest.log(`Starting tests '${test.name}'`);
    test();
});
