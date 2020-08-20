/**
 * Test the shell's --apiVersion and other options related to the MongoDB Versioned API, and
 * test passing API parameters to the Mongo() constructor.
 */

(function() {
'use strict';

const testCases = [
    // [requireApiVersion server parameter, expect success, command, API parameters]
    [false, true, {ping: 1}, {}],
    [false, true, {ping: 1}, {version: '1'}],
    [false, true, {count: 'collection'}, {version: '1'}],
    [false, true, {getLog: 'global'}, {version: '1'}],
    [false, true, {getLog: 'global'}, {version: '1', deprecationErrors: true}],
    // getLog isn't in API Version 1, so it's banned with strict: true.
    [false, false, {getLog: 'global'}, {version: '1', strict: true}],
    [false, true, {ping: 1}, {version: '1', strict: true}],
    [false, true, {testDeprecation: 1}, {version: '1', strict: true}],
    [false, false, {testDeprecation: 1}, {version: '1', deprecationErrors: true}],
    [false, false, {testDeprecation: 1}, {version: '1', strict: true, deprecationErrors: true}],
    // tests with setParameter requireApiVersion: true.
    [true, true, {count: 'collection'}, {version: '1'}],
    [true, true, {count: 'collection'}, {version: '1', strict: true}],
    [true, false, {ping: 1}, {}],
    [true, true, {ping: 1}, {version: '1'}],
];

function runShell(requireApiVersion, expectSuccess, command, api) {
    let shellArgs = [];
    if (api.version) {
        shellArgs.push('--apiVersion', api.version);
    }

    if (api.strict) {
        shellArgs.push('--apiStrict');
    }

    if (api.deprecationErrors) {
        shellArgs.push('--apiDeprecationErrors');
    }

    // Test runCommand and runReadCommand.
    const scripts = [
        `assert.commandWorked(db.getSiblingDB("admin").runCommand(${tojson(command)}))`,
        `assert.commandWorked(db.getSiblingDB("admin").runReadCommand(${tojson(command)}))`,
    ];

    for (const script of scripts) {
        jsTestLog(`Run shell with script ${script} and args ${tojson(shellArgs)},` +
                  ` requireApiVersion = ${requireApiVersion}, expectSuccess = ${expectSuccess}`);

        const result = runMongoProgram.apply(
            null, ['mongo', '--port', mongod.port, '--eval', script].concat(shellArgs || []));

        if (expectSuccess) {
            assert.eq(result,
                      0,
                      `Error running shell with command ${tojson(command)} and args ${
                          tojson(shellArgs)}`);
        } else {
            assert.neq(result,
                       0,
                       `Unexpected success running shell with` +
                           ` command ${tojson(command)} and args ${tojson(shellArgs)}`);
        }
    }
}

function newMongo(requireApiVersion, expectSuccess, command, api) {
    jsTestLog(`Construct Mongo object with command ${tojson(command)} and args ${tojson(api)},` +
              ` requireApiVersion = ${requireApiVersion}, expectSuccess = ${expectSuccess}`);
    if (expectSuccess) {
        const m = new Mongo(`mongodb://localhost:${mongod.port}`,
                            undefined /* encryptedDBClientCallback */,
                            {api: api});
        const reply = m.adminCommand(command);
        assert.commandWorked(reply, command);
    } else {
        let m;
        try {
            m = new Mongo(`mongodb://localhost:${mongod.port}`,
                          undefined /* encryptedDBClientCallback */,
                          {api: api});
        } catch (e) {
            // The constructor threw, but we expected failure.
            print(e);
            return;
        }
        const reply = m.adminCommand(command);
        assert.commandFailed(reply, command);
    }
}

const mongod = MongoRunner.runMongod({verbose: 2});

for (let [requireApiVersion, successExpected, command, api] of testCases) {
    const m = new Mongo(`localhost:${mongod.port}`, undefined, {api: {version: '1'}});
    assert.commandWorked(
        m.getDB("admin").runCommand({setParameter: 1, requireApiVersion: requireApiVersion}));

    runShell(requireApiVersion, successExpected, command, api);
    newMongo(requireApiVersion, successExpected, command, api);
}

// Reset.
const m = new Mongo(`localhost:${mongod.port}`, undefined, {api: {version: '1'}});
assert.commandWorked(m.getDB("admin").runCommand({setParameter: 1, requireApiVersion: false}));

/*
 * Shell-specific tests.
 */

// Version 2 is not supported.
runShell(false, false, {ping: 1}, {version: '2'});
// apiVersion is required if strict or deprecationErrors is included
runShell(false, false, {ping: 1}, {strict: true});
runShell(false, false, {ping: 1}, {deprecationErrors: true});
runShell(false, false, {ping: 1}, {strict: true, deprecationErrors: true});

/*
 * Mongo-specific tests.
 */
assert.throws(() => {
    new Mongo(mongod.port, null, "not an object");
}, [], "Mongo() constructor should check that options argument is an object");

assert.throws(() => {
    new Mongo(mongod.port, null, {api: "not an object"});
}, [], "Mongo() constructor should check that 'api' is an object");

assert.throws(() => {
    new Mongo(mongod.port, null, {api: {version: 1}});
}, [], "Mongo() constructor should reject API version 1 (as a number)");

assert.throws(() => {
    new Mongo(mongod.port, null, {api: {version: '2'}});
}, [], "Mongo() constructor should reject API version 2");

assert.throws(() => {
    new Mongo(mongod.port, null, {api: {version: '1', strict: 1}});
}, [], "Mongo() constructor should reject strict: 1");

assert.throws(() => {
    new Mongo(mongod.port, null, {api: {version: '1', strict: 'asdf'}});
}, [], "Mongo() constructor should reject strict: 'asdf");

assert.throws(() => {
    new Mongo(mongod.port, null, {api: {version: '1', deprecationErrors: 1}});
}, [], "Mongo() constructor should reject deprecationErrors: 1");

assert.throws(() => {
    new Mongo(mongod.port, null, {api: {version: '1', deprecationErrors: 'asdf'}});
}, [], "Mongo() constructor should reject deprecationErrors: 'asdf'");

// apiVersion is required if strict or deprecationErrors is included
assert.throws(() => {
    new Mongo(mongod.port, null, {api: {strict: true}});
}, [], "Mongo() constructor should reject 'strict' without 'version'");

assert.throws(() => {
    new Mongo(mongod.port, null, {api: {deprecationErrors: true}});
}, [], "Mongo() constructor should reject 'deprecationErrors' without 'version'");

MongoRunner.stopMongod(mongod);

/*
 * Test that we can call replSetGetStatus while assembling the shell prompt, although
 * replSetGetStatus is not in API Version 1 and the shell is running with --apiStrict.
 */
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const testPrompt = "assert(RegExp('PRIMARY').test(defaultPrompt()))";
const result = runMongoProgram('mongo',
                               '--port',
                               rst.getPrimary().port,
                               '--apiVersion',
                               '1',
                               '--apiStrict',
                               '--eval',
                               testPrompt);
assert.eq(result, 0, `Error running shell with script '${testPrompt}'`);

rst.stopSet();
})();
