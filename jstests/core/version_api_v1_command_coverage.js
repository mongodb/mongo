/**
 * Checks that commands included/omitted in API V1 behave correctly with various combinations of API
 * parameters.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: createUser, dropUser.
 *   not_allowed_with_security_token,
 *   requires_non_retryable_commands,
 *   uses_api_parameters,
 * ]
 */

let counter = 0;
const counter_fun = function() {
    return `APIV1-${counter}`;
};

const testDB = db.getSiblingDB(jsTestName());
const testColl = testDB.getCollection("test");

function runTest({cmd, apiVersion1, apiStrict, apiDeprecationErrors}) {
    // Instantiate "cmd" so we can modify it.
    let copy = cmd();
    jsTestLog(
        `Test ${tojson(copy)}, which is ${apiVersion1 ? "in" : "not in"} API V1, with apiStrict = ${
            apiStrict} and apiDeprecationErrors = ${apiDeprecationErrors}`);
    copy.apiVersion = "1";
    copy.apiStrict = apiStrict;
    copy.apiDeprecationErrors = apiDeprecationErrors;
    if (!apiVersion1 && apiStrict) {
        assert.commandFailedWithCode(
            testDB.runCommand(copy),
            ErrorCodes.APIStrictError,
            "Provided apiStrict: true, but the invoked command's apiVersions() does not include \"1\"");
    } else {
        assert.commandWorked(testDB.runCommand(copy));
    }
}

const commands = [
    {cmd: () => ({buildInfo: 1}), apiVersion1: false},
    {cmd: () => ({createUser: counter_fun(), pwd: "pwd", roles: []}), apiVersion1: false},
    {cmd: () => ({dropUser: counter_fun()}), apiVersion1: false},
    {cmd: () => ({serverStatus: 1}), apiVersion1: false},
    {cmd: () => ({usersInfo: 1}), apiVersion1: false},
    {cmd: () => ({aggregate: testColl.getName(), pipeline: [], cursor: {}}), apiVersion1: true},
    {cmd: () => ({count: "system.js"}), apiVersion1: true},
    {cmd: () => ({create: counter_fun()}), apiVersion1: true},
    {cmd: () => ({find: counter_fun()}), apiVersion1: true},
    {
        cmd: () => ({insert: "APIV1-0", documents: [{_id: counter_fun(), cast: "jonSnow"}]}),
        apiVersion1: true
    },
    {
        cmd: () => ({
            update: "APIV1-0",
            updates: [{q: {_id: counter_fun()}, u: {$set: {cast: "aryaStark"}}}]
        }),
        apiVersion1: true,
    },
    {
        cmd: () => ({delete: "APIV1-0", deletes: [{q: {_id: counter_fun()}, limit: 1}]}),
        apiVersion1: true
    },
    {cmd: () => ({drop: counter_fun()}), apiVersion1: true}
];

for (let {cmd, apiVersion1} of commands) {
    counter = 0;
    for (let apiStrict of [false, true]) {
        for (let apiDeprecationErrors of [false, true]) {
            runTest({
                cmd: cmd,
                apiVersion1: apiVersion1,
                apiStrict: apiStrict,
                apiDeprecationErrors: apiDeprecationErrors
            });
            counter += 1;
        }
    }
}
