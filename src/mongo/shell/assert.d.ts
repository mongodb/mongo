// type declarations for assert.js

/**
 * Throws test exception with message.
 * 
 * @param msg Failure message
 * @param errorCodeOrObj Error object to reference in the exception.
 * @throws {Error}
 * 
 * @example
 * switch (scenario.type) {
 *   case 'A':
 *     assert.eq(scenario.result, 1);
 *     break;
 *   case 'B':
 *     assert.eq(scenario.result, 2);
 *     break;
 *   default:
 *     doassert('scenario was not type A or B');
 * }
 */
declare function doassert(
    msg: string | function | object,
    errorCodeOrObj?: number | BulkWriteResult | BulkWriteError | WriteResult | {
        code?: any,
        writeErrors?: any,
        errorLabels?: any,
        writeConcernError?: any,
        writeConcernErrors?: any,
        hasWriteConcernError?: any,
    })
    : never

declare module assert {

    /**
     * Assert equality.
     *
     * Equality is based on '`==`', with a fallthrough to comparing JSON representations.
     * This is not a strict equality (`===`) assertion.
     * 
     * @param a 
     * @param b 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @example
     * const actual = getValue();
     * const expected = 'foobar';
     * assert.eq(actual, expected);
     * 
     * @throws AssertionError upon failure.
     */
    export function eq(a, b, msg?: string | function | object, attr?: object): void
    export function neq(a, b, msg, attr): void

    export function lt(a, b, msg, attr): void
    export function gt(a, b, msg, attr): void
    export function lte(a, b, msg, attr): void
    export function gte(a, b, msg, attr): void

    export function isnull(what, msg, attr): void

    export function docEq(expectedDoc, actualDoc, msg, attr): void

    export function setEq(expectedSet, actualSet, msg, attr): void
    export function sameMembers(aArr, bArr, msg, compareFn, attr): void
    export function fuzzySameMembers(aArr, bArr, fuzzyFields, msg, places, attr): void

    export function hasFields(result, arr, msg, attr): void

    export function includes(haystack, needle, msg, attr): void
    export function contains(o, arr, msg, attr): void
    export function doesNotContain(o, arr, msg, attr): void
    export function containsPrefix(prefix, arr, msg, attr): void

    export function time(f, msg, timeout, opts, attr): any

    export function soon(func, msg, timeout, interval, opts, attr): void
    export function soonNoExcept(func, msg, timeout, interval, opts, attr): void

    export function soonRetryOnNetworkErrors(func, msg, timeout, interval, opts, attr): void
    export function soonRetryOnAcceptableErrors(func, acceptableErrors, msg, timeout, interval, opts, attr): void

    export function retry(func, msg, num_attempts, intervalMS, opts, attr): void
    export function retryNoExcept(func, msg, num_attempts, intervalMS, opts, attr): void

    export function throws(func, params, msg, attr): Error
    export function throwsWithCode(func, expectedCode, params, msg, attr): Error
    export function doesNotThrow(func, params, msg, attr): any
    export function dropExceptionsWithCode(func, dropCodes, onDrop): any

    /**
     * Assert that a command worked by testing a result object.
     * 
     * @param res Result that should be successful ("worked").
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * 
     * @example
     * const dbTest = db.getSiblingDB(jsTestName());
     * const res = dbTest.createCollection("coll1");
     * assert.commandWorked(res);
     * 
     * @throws AssertionError upon failure.
     * @returns The result object to continue any chaining.
     */
    export function commandWorked(
        res: WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError,
        msg?: string | function | object)
        : object;

    export function commandWorkedIgnoringWriteErrors(res, msg): object
    export function commandWorkedIgnoringWriteConcernErrors(res, msg): object
    export function commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res, msg): object
    export function commandWorkedOrFailedWithCode(res, errorCodeSet, msg): object
    export function commandFailed(res, msg): object
    export function commandFailedWithCode(res, expectedCode, msg): object
    export function adminCommandWorkedAllowingNetworkError(node, commandObj): any

    export function writeOK(res, msg, attr): any
    export function writeError(res, msg): any
    export function writeErrorWithCode(res, expectedCode, msg): any

    export function between(a, b, c, msg, inclusive, attr): void
    export function betweenIn(a, b, c, msg, attr): void
    export function betweenEx(a, b, c, msg, attr): void

    export function close(a, b, msg, places): void
    export function closeWithinMS(a, b, msg, deltaMS, attr): void

    export function noAPIParams(cmdOptions): void
}
