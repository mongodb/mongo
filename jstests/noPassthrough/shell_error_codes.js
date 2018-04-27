/**
 * Tests for the ErrorCodes objects in error_codes.js generated file.
 */
(() => {
    "use strict";

    const tests = [];

    const nonExistingErrorCode = 999999999;

    tests.push(function errorCodesShouldThrowExceptionForNonExistingError() {
        assert.throws(() => {
            return ErrorCodes.thisIsAnErrorCodeThatDoesNotExist;
        });
    });

    tests.push(function errorCodesShouldNotThrowExceptionForExistingError() {
        assert.doesNotThrow(() => {
            return ErrorCodes.BadValue;
        });
    });

    tests.push(function errorCodesShouldNotThrowExceptionForInheritedAttributes() {
        assert.doesNotThrow(() => {
            return ErrorCodes.prototype;
        });
    });

    tests.push(function errorCodesShouldNotThrowExceptionForSymbols() {
        assert.doesNotThrow(() => {
            return print(+ErrorCodes);
        });
    });

    tests.push(function errorCodesShouldNotThrowExceptionForConstructor() {
        assert.doesNotThrow(() => {
            return ErrorCodes.constructor;
        });
    });

    tests.push(function errorCodeStringsShouldThrowExceptionForNonExistingError() {
        assert.throws(() => {
            return ErrorCodeStrings[nonExistingErrorCode];
        });
    });

    tests.push(function errorCodeStringsShouldNotThrowExceptionForExistingError() {
        assert.doesNotThrow(() => {
            return ErrorCodeStrings[2];
        });
    });

    tests.push(function errorCodesShouldHaveCategoriesDefined() {
        assert.eq(true, ErrorCodes.isNetworkError(ErrorCodes.HostNotFound));
    });

    tests.push(function errorCodesCategoriesShouldReturnFalseOnNonExistingErrorCodes() {
        assert.eq(false, ErrorCodes.isNetworkError(nonExistingErrorCode));
    });

    /* main */
    tests.forEach((test) => {
        jsTest.log(`Starting tests '${test.name}'`);
        test();
    });
})();
