// type declarations for assert.js

/**
 * Throws test exception with message.
 * 
 * @param msg Failure message
 * @param errorCodeOrObj Error object to reference in the exception.
 * 
 * @throws {Error} if assertion is not satisfied.
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
    }
): never

/**
 * Assert that a value is true.
 * 
 * This is a "truthy" condition test for any object/type, not just booleans (ie, not `=== true`).
 * 
 * Consider using more specific methods of the assert module, such as `assert.eq`,
 * which produce richer failure diagnostics.
 * 
 * @param value Value under test
 * @param msg Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param attr Additional attributes to be included in failure messages.
 * 
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert(coll.drop())
 */
declare function assert(value: boolean | any, msg?: string | function | object, attr?: object): void

/**
 * Sort document object fields.
 * 
 * @param doc 
 * 
 * @returns Sorted document object.
 */
declare function sortDoc(doc: object): any

declare module assert {

    /**
     * Assert equality.
     *
     * Equality is based on '`==`', with a fallthrough to comparing JSON representations.
     * This is not a strict equality (`===`) assertion.
     * 
     * @param a Left-hand side operand
     * @param b Right-hand side operand
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const actual = getValue();
     * const expected = 'foobar';
     * assert.eq(actual, expected);
     */
    function eq(a, b, msg?: string | function | object, attr?: object): void

    /**
     * Assert inequality.
     *
     * Inequality is based on '`a != b`', with a fallthrough to comparing JSON representations.
     * This is not a strict equality (`a !== b`) assertion.
     * 
     * @param a Left-hand side operand
     * @param b Right-hand side operand
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const actual = getValue();
     * const forbidden = 'foobar';
     * assert.neq(actual, forbidden);
     */
    function neq(a, b, msg?: string | function | object, attr?: object): void

    /**
     * Assert that a < b.
     * 
     * @param a Left-hand side operand
     * @param b Right-hand side operand
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const actual = getValue();
     * const ceiling = 12;
     * assert.lt(actual, ceiling);
     */
    function lt(a, b, msg?: string | function | object, attr?: object): void

    /**
     * Assert that a > b.
     * 
     * @param a Left-hand side operand
     * @param b Right-hand side operand
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const actual = getValue();
     * const floor = 12;
     * assert.gt(actual, floor);
     */
    function gt(a, b, msg?: string | function | object, attr?: object): void

    /**
     * Assert that a <= b.
     * 
     * @param a Left-hand side operand
     * @param b Right-hand side operand
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const actual = getValue();
     * const ceiling = 12;
     * assert.lte(actual, ceiling);
     */
    function lte(a, b, msg?: string | function | object, attr?: object): void

    /**
     * Assert that a >= b.
     * 
     * @param a Left-hand side operand
     * @param b Right-hand side operand
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const actual = getValue();
     * const floor = 12;
     * assert.gte(actual, floor);
     */
    function gte(a, b, msg?: string | function | object, attr?: object): void

    /**
     * Assert that a value is null.
     * 
     * @param value Value under test
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const value = getValue();
     * assert.isnull(value);
     */
    function isnull(value, msg?: string | function | object, attr?: object): void

    /**
     * Assert equality of document objects.
     * 
     * The order of fields (properties) within objects is ignored.
     * The bsonUnorderedFieldsCompare function is leveraged.
     * 
     * @param docA Document object
     * @param docB Document object
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.docEq(results[0], {_id: null, result: ["abc"]})
     */
    function docEq(docA, docB, msg?: string | function | object, attr?: object): void

    /**
     * Assert set equality.
     * 
     * @param setA 
     * @param setB 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.setEq(new Set([7, 8, 9, 10]), new Set(matchingIds))
     */
    function setEq(setA: Set, setB: Set, msg?: string | function | object, attr?: object): void

    /**
     * Assert that array have the same members.
     * 
     * Order of the elements is ignored.
     * 
     * @param aArr 
     * @param bArr 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param compareFn Custom element-comparison function
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.sameMembers(res, [{_id: 1, count: 500}, {_id: 2, count: 5001}]);
     */
    function sameMembers(aArr: any[], bArr: any[], msg?: string | function | object, compareFn?: function, attr?: object): void

    /**
     * 
     * @param aArr 
     * @param bArr 
     * @param fuzzyFields 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param places 
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     */
    function fuzzySameMembers(aArr, bArr, fuzzyFields, msg, places?: number, attr?: object): void

    /**
     * Assert that an object has specific fields.
     * 
     * @param value 
     * @param arr 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.hasFields(result.serverInfo, ['host', 'port', 'version', 'gitVersion']);
     */
    function hasFields(value: object, arr: any[], msg?: string | function | object, attr?: object): void

    /**
     * Assert that the "haystack" includes the "needle".
     * 
     * This defers to the builtin "includes" method of the "haystack" object,
     * which might be an array (for element containment), string (for substring-matching), etc.
     * 
     * @param haystack 
     * @param needle 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.includes(res.message, "$merge failed due to a DuplicateKey error");
     */
    function includes(haystack, needle, msg?: string | function | object, attr?: object): void

    /**
     * Assert that an array contains a specific element.
     * 
     * @param element Element to be found
     * @param arr Array to search
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.contains(res.nMatched, [0, 1]);
     */
    function contains(element, arr: any[], msg?: string | function | object, attr?: object): void

    /**
     * Assert that an array does not contain a specific element.
     * 
     * @param element Element to not be found
     * @param arr Array to search
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.doesNotContain(errorCode, [401, 404, 500]);
     */
    function doesNotContain(element, arr: any[], msg?: string | function | object, attr?: object): void

    /**
     * Assert that an array contains a string that starts with a prefix.
     * 
     * @param prefix 
     * @param arr 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.containsPrefix("Detected a time-series bucket with mixed schema data", res.warnings);
     */
    function containsPrefix(prefix: string, arr: any[], msg?: string | function | object, attr?: object): void

    /**
     * Assert that function execution completes within a specified timeout.
     * 
     * @param func Function to be executed, or string to be `eval`ed.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param timeout Timeout in ms, default 30_000.
     * @param opts Options to control hang analyzer via `runHangAnalyzer` property.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @returns Result of the function evaluation/execution.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * let ok = assert.time(() => {
     *     const testDB = db.getMongo().getDB('test');
     *     const res = testDB.runCommand(
     *         {usersInfo: user.userName, maxTimeMS: 30_000});
     *     return res.ok;
     *  });
     *  assert(ok);
     */
    function time(func: function | string, msg?: string | function | object, timeout?: number, opts?: {runHangAnalyzer: boolean}, attr?: object): any

    /**
     * Assert that a function eventually evaluates to true.
     * 
     * Calls a function 'func' at repeated intervals until either func() returns true
     * or more than 'timeout' milliseconds have elapsed.
     * 
     * @param func Function to be executed, or string to be `eval`ed.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param timeout Timeout in ms. In CI, this is 10min, otherwise 90sec.
     * @param interval Interval in ms to wait between tries, default 200ms.
     * @param opts Options to control hang analyzer via `runHangAnalyzer` property.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.soon(() => changeStream.hasNext());
     */
    function soon(func: function | string, msg?: string | function | object, timeout?: number, interval?: number, opts?: {runHangAnalyzer: boolean}, attr?: object): void

    /**
     * Assert that a function eventually evaluates to true.
     * 
     * This is a special case of {@link assert.soon}.
     * 
     * Calls a function 'func' at repeated intervals until either func() returns true
     * or more than 'timeout' milliseconds have elapsed. Exceptions are allowed and suppressed.
     * 
     * @param func Function to be executed, or string to be `eval`ed.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param timeout Timeout in ms. In CI, this is 10min, otherwise 90sec.
     * @param interval Interval in ms to wait between tries, default 200ms.
     * @param opts Options to control hang analyzer via `runHangAnalyzer` property.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.soonNoExcept(() => {
     *    let numIndices = secondaryDB.getCollection(collectionName).getIndexes().length;
     *    // this might fail/throw a few times, but that is okay
     *    assert.eq(numIndices, 4);
     *    return true;
     * });
     */
    function soonNoExcept(func, msg?: string | function | object, timeout?: number, interval?: number, opts?: {runHangAnalyzer: boolean}, attr?: object): void

    /**
     * Assert that a function eventually evaluates to true, retrying on Network errors.
     * 
     * This is a special case of {@link assert.soonRetryOnAcceptableErrors}.
     * 
     * @param func Function to be executed, or string to be `eval`ed.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param timeout Timeout in ms. In CI, this is 10min, otherwise 90sec.
     * @param interval Interval in ms to wait between tries, default 200ms.
     * @param opts Options to control hang analyzer via `runHangAnalyzer` property.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.soonRetryOnNetworkErrors(() => {
     *     primaryInfo = db.isMaster();
     *     return primaryInfo.hasOwnProperty("ismaster") && primaryInfo.ismaster;
     * });
     */
    function soonRetryOnNetworkErrors(func, msg?: string | function | object, timeout?: number, interval?: number, opts?: {runHangAnalyzer: boolean}, attr?: object): void

    /**
     * Assert that a function eventually evaluates to true, retrying on any acceptable errors.
     * 
     * This is a special case of {@link assert.soon}.
     * 
     * @param func Function to be executed, or string to be `eval`ed.
     * @param acceptableErrors Error (or array of Errors) that are allowed.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param timeout Timeout in ms. In CI, this is 10min, otherwise 90sec.
     * @param interval Interval in ms to wait between tries, default 200ms.
     * @param opts Options to control hang analyzer via `runHangAnalyzer` property.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.soonRetryOnAcceptableErrors(() => {
     *     assert.commandWorked(
     *         db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}));
     *     return true;
     * }, ErrorCodes.FailedToSatisfyReadPreference);
     */
    function soonRetryOnAcceptableErrors(func, acceptableErrors: Error | Error[], msg?: string | function | object, timeout?: number, interval?: number, opts?: {runHangAnalyzer: boolean}, attr?: object): void

    /**
     * Assert that a function eventually evaluates to true.
     * 
     * This calls a function up to a specified number of times, whereas {@link assert.soon}
     * calls the function until a timeout is exceeded.
     * 
     * @param func Function to be executed.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param num_attempts Number of attempts to try the function execution.
     * @param intervalMS Interval in ms to wait between tries, default 0.
     * @param opts Options to control hang analyzer via `runHangAnalyzer` property.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.retry(
     *     () => db.serverStatus().metrics.cursor.open.pinned == 0,
     *     "Expected 0 pinned cursors, but have " + tojson(db.serverStatus().metrics.cursor),
     *     10);
     */
    function retry(func: function, msg: string | function | object, num_attempts: number, intervalMS?: number, opts?: {runHangAnalyzer: boolean}, attr?): void

    /**
     * Assert that a function eventually evaluates to true, ignoring exceptions.
     * 
     * Special case of {@link assert.retry} where the function is executed "safely" within
     * a try/catch to continue retries.
     * 
     * @param func Function to be executed.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param num_attempts Number of attempts to try the function execution.
     * @param intervalMS Interval in ms to wait between tries, default 0.
     * @param opts Options to control hang analyzer via `runHangAnalyzer` property.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.retryNoExcept(() => {
     *     assert.commandWorked(configDB.chunks.update({_id: chunkDoc._id}, {$set: {jumbo: true}}));
     *     return true;
     * }, "Setting jumbo flag update failed on config server", 10);
     */
    function retryNoExcept(func, msg?: string | function | object, num_attempts: number, intervalMS?: number, opts?: {runHangAnalyzer: boolean}, attr?): void

    /**
     * Assert that a function throws an exception.
     * 
     * @param func Function to be executed.
     * @param params Parameters to apply into the function execution.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @returns {Error} that the function threw.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.throws(() => local.aggregate(pipeline));
     */
    function throws(func: function, params?: any[], msg?: string | function | object, attr?: object): Error

    /**
     * Assert that a function throws an exception matching a specific error code.
     * 
     * This is an extension of {@link assert.throws}.
     * 
     * @param func Function to be executed.
     * @param expectedCode Code (or array of possible Codes to match) to match on the
     *                     `code` field of the resulting error.
     * @param params Parameters to apply into the function execution.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @returns {Error} that the function threw.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.throwsWithCode(() => coll.aggregate({$project: {x: "$$ref"}}).toArray(), 17276);
     */
    function throwsWithCode(func: function, expectedCode: number | number[], params?: any[], msg?: string | function | object, attr?: object): Error

    /**
     * Assert that a function does not throw an exception.
     * 
     * This is typically used when the test wants to verify that a function executes safely,
     * but does not warrant any further verifications of its output or effects.
     * 
     * @param func Function to be executed.
     * @param params Parameters to apply into the function execution.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @returns The output of the function.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.doesNotThrow(() => source.aggregate(pipeline, options));
     */
    function doesNotThrow(func: function, params?: any[], msg?: string | function | object, attr?: object): any

    /**
     * Assert that a function throws an exception matching a specific error code,
     * and trigger a callback function with those as inputs.
     * 
     * This is an extension of {@link assert.throwsWithCode}.
     * 
     * @param func Function to be executed.
     * @param dropCodes Code (or array of possible Codes to match) that are expected to be thrown.
     * @param onDrop Function to execute on matched {@link dropCodes}.
     * 
     * @returns The output of {@onDrop}, invoked with exceptions matching {@link dropCodes}
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.dropExceptionsWithCode(
     *     () => runBackgroundDbCheck(hosts),
     *     ErrorCodes.Interrupted,
     *     (e) => jsTestLog("Skipping dbCheck due to transient error: " + tojson(e)));
     */
    function dropExceptionsWithCode(func: function, dropCodes: number | number[], onDrop: function): any

    /**
     * Assert that a command worked by testing a result object.
     * 
     * @param res Result that should be successful ("worked").
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * 
     * @returns The result object to continue any chaining.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const dbTest = db.getSiblingDB(jsTestName());
     * const res = dbTest.createCollection("coll1");
     * assert.commandWorked(res);
     */
    function commandWorked(
        res: WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError,
        msg?: string | function | object
    ): object

    /**
     * Assert that a command worked, ignoring write errors.
     * 
     * This is an extension of {@link assert.commandWorked}.
     * 
     * @param res Result that should be successful ("worked").
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * 
     * @returns The result object to continue any chaining.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const dbTest = db.getSiblingDB(jsTestName());
     * const res = dbTest.createCollection("coll1");
     * assert.commandWorkedIgnoringWriteErrors(res);
     */
    function commandWorkedIgnoringWriteErrors(
        res: WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError,
        msg?: string | function | object
    ): object

    /**
     * Assert that a command worked, ignoring write concern errors.
     * 
     * @param res Result that should be successful ("worked").
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * 
     * @returns The result object to continue any chaining.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const dbTest = db.getSiblingDB(jsTestName());
     * const res = dbTest.createCollection("coll1");
     * assert.commandWorkedIgnoringWriteConcernErrors(res);
     */
    function commandWorkedIgnoringWriteConcernErrors(
        res: WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError,
        msg?: string | function | object
    ): object

    /**
     * Assert that a command worked, ignoring write errors and write concern errors.
     * 
     * @param res Result that should be successful ("worked").
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * 
     * @returns The result object to continue any chaining.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const dbTest = db.getSiblingDB(jsTestName());
     * const res = dbTest.createCollection("coll1");
     * assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
     */
    function commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(
        res: WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError,
        msg?: string | function | object
    ): object

    /**
     * Assert that a command worked, or otherwise failed with a specific code.
     * 
     * This is a convenience wrapper around {@link assert.commandWorked}
     * and {@link assert.commandFailedWithCode}.
     * 
     * @param res Result that should be successful ("worked").
     * @param errorCodeSet Code (or array of possible codes to match) on failed results.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * 
     * @returns The result object to continue any chaining.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const dbTest = db.getSiblingDB(jsTestName());
     * const res = dbTest.createCollection("coll1");
     * assert.commandWorkedOrFailedWithCode(res. 58712);
     */
    function commandWorkedOrFailedWithCode(
        res: WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError,
        errorCodeSet: number | number[],
        msg?: string | function | object
    ): object

    /**
     * Assert that a command failed.
     * 
     * @param res Result that should be successful ("worked").
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * 
     * @returns The result object to continue any chaining.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const dbTest = db.getSiblingDB(jsTestName());
     * const res = dbTest.createCollection("coll1");
     * assert.commandFailed(res);
     */
    function commandFailed(
        res: WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError,
        msg?: string | function | object
    ): object

    /**
     * Assert that a command failed with a specific code.
     * 
     * @param res Result that should have failed.
     * @param expectedCode Code (or array of possible codes to match)
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * 
     * @returns The result object to continue any chaining.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const dbTest = db.getSiblingDB(jsTestName());
     * const res = dbTest.createCollection("coll1");
     * assert.commandFailedWithCode(res, 17260);
     */
    function commandFailedWithCode(
        res: WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError,
        expectedCode: number | number[],
        msg?: string | function | object
    ): object

    /**
     * Asserts that a command run on the 'admin' database worked, ignoring network errors.
     *
     * Returns the response if the command succeeded, or undefined if the command failed, *even* if
     * the failure was due to a network error.
     * 
     * @param node 
     * @param commandObj Command object to be called inside `node.adminCommand`.
     * 
     * @returns The result object to continue any chaining.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.adminCommandWorkedAllowingNetworkError(replTest.getPrimary(), {replSetReconfig: config});
     */
    function adminCommandWorkedAllowingNetworkError(node, commandObj): any

    /**
     * Assert that a command resulted in successful writes.
     * 
     * @param res Result object.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @returns The result object to continue any chaining.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const dbTest = db.getSiblingDB(jsTestName());
     * const res = dbTest.createCollection("coll1");
     * assert.writeOK(res);
     */
    function writeOK(
        res: WriteResult | BulkWriteResult | WriteCommandError | WriteError | BulkWriteError,
        msg?: string | function | object,
        attr?: {ignoreWriteConcernErrors}
    ): WriteResult | BulkWriteResult

    /**
     * Assert that a command resulted in write errors.
     * 
     * @param res Result object.
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * 
     * @returns The result object to continue any chaining.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const dbTest = db.getSiblingDB(jsTestName());
     * const res = dbTest.createCollection("coll1");
     * assert.writeError(res);
     */
    function writeError(
        res: WriteResult | BulkWriteResult | WriteCommandError | WriteError | BulkWriteError,
        msg?: string | function | object
    ): WriteCommandError | WriteError | BulkWriteError

    /**
     * Assert that a command resulted in write errors matching specific Codes.
     * 
     * This is a stricter check of {@link assert.writeError}.
     * 
     * @param res Result object.
     * @param expectedCode Code (or array of possible codes to match)
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * 
     * @returns The result object to continue any chaining.
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * const dbTest = db.getSiblingDB(jsTestName());
     * const res = dbTest.createCollection("coll1");
     * assert.writeErrorWithCode(ex, ErrorCodes.DatabaseDropPending);
     */
    function writeErrorWithCode(
        res: WriteResult | BulkWriteResult | WriteCommandError | WriteError | BulkWriteError,
        expectedCode: number | number[],
        msg?: string | function | object
    ): WriteCommandError | WriteError | BulkWriteError

    /**
     * Assert that a <= b <= c, or a < b < c.
     * 
     * @param a 
     * @param b 
     * @param c 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param inclusive Whether to use inclusive (<=) comparisons, default true.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.between(1000, totalSpillingStats.spilledBytes, 100000);
     */
    function between(a, b, c, msg?: string | function | object, inclusive?: boolean, attr?: object): void

    /**
     * Assert that a <= b <= c, inclusively.
     * 
     * This is a convenience wrapper around {@link assert.between}.
     * 
     * @param a 
     * @param b 
     * @param c 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.between(1000, totalSpillingStats.spilledBytes, 100000);
     */
    function betweenIn(a, b, c, msg?: string | function | object, attr?: object): void

    /**
     * Assert that a < b < c, exclusively.
     * 
     * This is a convenience wrapper around {@link assert.between}.
     * 
     * @param a 
     * @param b 
     * @param c 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.between(1000, totalSpillingStats.spilledBytes, 100000);
     */
    function betweenEx(a, b, c, msg?: string | function | object, attr?: object): void

    /**
     * Assert that numerical values are equivalent to within significant figures.
     * 
     * @param a 
     * @param b 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param places Number of significant figures to allow for tolerance, default 4.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.close(res.pop, popExpected, '', 10);
     */
    function close(a: number, b: number, msg?: string | function | object, places?: number): void

    /**
     * Asserts if the times in millis are equal to within a tolerance.
     * 
     * @param a 
     * @param b 
     * @param msg Failure message, displayed when the assertion fails.
     *            If a function, it is invoked and its result is used as the failure message.
     *            If an object, its conversion to json is used as the failure message.
     * @param deltaMS Tolerance in milliseconds, default 1000 ms.
     * @param attr Additional attributes to be included in failure messages.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.closeWithinMS(startTime,
     *                     latestStartUpLog.startTime,
     *                     "StartTime doesn't match one from _id",
     *                     2000); // Expect less than 2 sec delta
     */
    function closeWithinMS(a: Date | number, b: Date | number, msg?: string | function | object, deltaMS?: number, attr?: object): void

    /**
     * Assert that a command object does not have any of the fields
     * `apiVersion`, `apiStruct`, or `apiDeprecationErrors`.
     * 
     * @param cmdOptions Object to validate.
     *                   If it is not an Object, the assertion passes.
     * 
     * @throws {Error} if assertion is not satisfied.
     * 
     * @example
     * assert.noAPIParams(options);
     */
    function noAPIParams(cmdOptions): void
}
