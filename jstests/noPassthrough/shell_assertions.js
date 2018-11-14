/**
 * Tests for the assertion functions in mongo/shell/assert.js.
 */
(() => {
    "use strict";

    const tests = [];

    const kDefaultTimeoutMS = 10 * 1000;
    const kSmallTimeoutMS = 200;
    const kSmallRetryIntervalMS = 1;
    const kDefaultRetryAttempts = 5;

    /* doassert tests */

    tests.push(function callingDoAssertWithStringThrowsException() {
        const expectedError = 'hello world';
        const actualError = assert.throws(() => {
            doassert(expectedError);
        });

        assert.eq('Error: ' + expectedError,
                  actualError,
                  'doAssert should throw passed msg as exception');
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

        assert.eq('Error: ' + expectedError,
                  actualError,
                  'doAssert should throw passed msg as exception');
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

    /* assert tests */

    tests.push(function assertShouldFailForMoreThan2Args() {
        const err = assert.throws(() => {
            assert(1, 2, 3);
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
        assert.neq(-1,
                   err.message.indexOf(message),
                   'assert message should should contain original message');
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

    /* assert.automsg tests */

    tests.push(function automsgShouldPassToAssert() {
        const defaultMessage = '1 === 2';
        const err = assert.throws(() => {
            assert.automsg(defaultMessage);
        });

        assert.neq(-1, err.message.indexOf(defaultMessage), 'default message should be returned');
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

    /* assert.eq.automsg tests */

    tests.push(function eqAutomsgShouldCreateMessage() {
        const defaultMessage = '[1] != [2]';
        const err = assert.throws(() => {
            assert.eq.automsg(1, 2);
        });

        assert.neq(-1, err.message.indexOf(defaultMessage), 'default message should be returned');
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
            }, 'assert message', kSmallTimeoutMS, kSmallRetryIntervalMS);
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
            }, 'assert message', kSmallTimeoutMS, kSmallRetryIntervalMS);
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
            }, 'assert message', kDefaultRetryAttempts, kSmallRetryIntervalMS);
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
            }, 'assert message', kDefaultRetryAttempts, kSmallRetryIntervalMS);
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
            }, 'assert message', -5 * 60 * 1000);
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

    /* main */

    tests.forEach((test) => {
        jsTest.log(`Starting tests '${test.name}'`);
        test();
    });
})();
