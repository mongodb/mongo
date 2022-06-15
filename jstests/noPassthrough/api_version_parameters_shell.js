/**
 * Test the shell's --apiVersion and other options related to the MongoDB Stable API, and
 * test passing API parameters to the Mongo() constructor.
 *
 * @tags: [
 *   requires_persistence,
 * ]
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

function runShellWithScript(port, requireApiVersion, expectSuccess, script, api) {
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

    jsTestLog(`Run shell with script ${script} and args ${tojson(shellArgs)},` +
              ` requireApiVersion = ${requireApiVersion}, expectSuccess = ${expectSuccess}`);

    const result = runMongoProgram.apply(
        null, ['mongo', '--port', port, '--eval', script].concat(shellArgs || []));

    if (expectSuccess) {
        assert.eq(
            result,
            0,
            `Error running shell with script ${tojson(script)} and args ${tojson(shellArgs)}`);
    } else {
        assert.neq(result,
                   0,
                   `Unexpected success running shell with` +
                       ` script ${tojson(script)} and args ${tojson(shellArgs)}`);
    }
}

function runShellWithCommand(port, requireApiVersion, expectSuccess, command, api) {
    // Test runCommand and runReadCommand.
    const scripts = [
        `assert.commandWorked(db.getSiblingDB("admin").runCommand(${tojson(command)}))`,
        `assert.commandWorked(db.getSiblingDB("admin").runReadCommand(${tojson(command)}))`,
    ];

    for (const script of scripts) {
        runShellWithScript(port, requireApiVersion, expectSuccess, script, api);
    }
}

function newMongo(port, requireApiVersion, expectSuccess, command, api) {
    jsTestLog(`Construct Mongo object with command ${tojson(command)} and args ${tojson(api)},` +
              ` requireApiVersion = ${requireApiVersion}, expectSuccess = ${expectSuccess}`);
    if (expectSuccess) {
        const m = new Mongo(
            `mongodb://localhost:${port}`, undefined /* encryptedDBClientCallback */, {api: api});
        const reply = m.adminCommand(command);
        assert.commandWorked(reply, command);
    } else {
        let m;
        try {
            m = new Mongo(`mongodb://localhost:${port}`,
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

    runShellWithCommand(mongod.port, requireApiVersion, successExpected, command, api);
    newMongo(mongod.port, requireApiVersion, successExpected, command, api);
}

let m = new Mongo(`localhost:${mongod.port}`, undefined, {api: {version: '1'}});
assert.commandWorked(m.getDB('admin').runCommand(
    {insert: 'collection', documents: [{}, {}, {}, {}, {}, {}], apiVersion: '1'}));

for (let requireApiVersion of [false, true]) {
    // Omit api = {}, that's tested elsewhere.
    for (let api of [{version: '1'},
                     {version: '1', strict: true},
                     {version: '1', deprecationErrors: true},
                     {version: '1', strict: true, deprecationErrors: true},
    ]) {
        assert.commandWorked(
            m.getDB("admin").runCommand({setParameter: 1, requireApiVersion: requireApiVersion}));

        /*
         * Test getMore. Create a cursor with the right API version parameters. Use an explicit lsid
         * to override implicit session creation.
         */
        const lsid = UUID();
        const versionedConn = new Mongo(`localhost:${mongod.port}`, undefined, {api: api});
        const findReply = assert.commandWorked(versionedConn.getDB('admin').runCommand(
            {find: 'collection', batchSize: 1, lsid: {id: lsid}}));
        const getMoreCmd = {
            getMore: findReply.cursor.id,
            collection: 'collection',
            lsid: {id: lsid},
            batchSize: 1
        };

        runShellWithCommand(
            mongod.port, requireApiVersion, true /* expectSuccess */, getMoreCmd, api);
        newMongo(mongod.port, requireApiVersion, true /* expectSuccess */, getMoreCmd, api);

        /*
         * Test unacknowledged writes (OP_MSG with moreToCome).
         */
        versionedConn.getDB('admin')['collection2'].insertMany([{}, {}]);
        const deleteScript = "db.getSiblingDB('admin').collection2.deleteOne({}, {w: 0});";
        runShellWithScript(
            mongod.port, requireApiVersion, true /* expectSuccess */, deleteScript, api);
        // The delete succeeds, collection2 had 2 records, it soon has 1. Use assert.soon since the
        // write is unacknowledged. Use countDocuments which is apiStrict-compatible.
        assert.soon(() => {
            return 1 === versionedConn.getDB('admin')['collection2'].countDocuments({});
        });
        const deleteCmd = {
            delete: 'collection2',
            deletes: [{q: {}, limit: 1}],
            writeConcern: {w: 0}
        };
        newMongo(mongod.port, requireApiVersion, true /* expectSuccess */, deleteCmd, api);
        // The delete succeeds, collection2 soon has 0 records.
        assert.soon(() => {
            return 0 === versionedConn.getDB('admin')['collection2'].countDocuments({});
        });
    }
}

/*
 * Shell-specific tests.
 */

// Version 2 is not supported.
runShellWithCommand(mongod.port, false, false, {ping: 1}, {version: '2'});
// apiVersion is required if strict or deprecationErrors is included
runShellWithCommand(mongod.port, false, false, {ping: 1}, {strict: true});
runShellWithCommand(mongod.port, false, false, {ping: 1}, {deprecationErrors: true});
runShellWithCommand(mongod.port, false, false, {ping: 1}, {strict: true, deprecationErrors: true});

if (m.adminCommand('buildinfo').modules.indexOf('enterprise') > -1) {
    /*
     * Test that we can call buildinfo while assembling the shell prompt, in order to determine that
     * this is MongoDB Enterprise, although buildinfo is not in API Version 1 and the shell is
     * running with --apiStrict.
     */
    const testPrompt = "assert(RegExp('MongoDB Enterprise').test(defaultPrompt()))";
    const result = runMongoProgram(
        'mongo', '--port', mongod.port, '--apiVersion', '1', '--apiStrict', '--eval', testPrompt);
    assert.eq(result, 0, `Error running shell with script '${testPrompt}'`);
}

// Test that shell helpers which return a cursor do not accept API versioning parameters. This is to
// avoid the potential pitfall of sending API parameters on the initial command but not on
// subsequent getMore's.
const coll = m.getDB("test").collection;
assert.throws(() => coll.aggregate([], {apiVersion: 1}));
assert.throws(() => coll.aggregate([], {apiStrict: true}));
assert.throws(() => coll.aggregate([], {apiDeprecationErrors: true}));
assert.throws(() => coll.find({}, {}, 1, 0, 1, {apiVersion: 1}));
assert.throws(() => coll.find({}, {}, 1, 0, 1, {apiStrict: true}));
assert.throws(() => coll.find({}, {}, 1, 0, 1, {apiDeprecationErrors: true}));
assert.throws(() => coll.watch({}, {apiVersion: 1}));
assert.throws(() => coll.watch({}, {apiStrict: true}));
assert.throws(() => coll.watch({}, {apiDeprecationErrors: true}));
assert.throws(() => coll.getDB().watch({}, {apiVersion: 1}));
assert.throws(() => coll.getDB().watch({}, {apiStrict: true}));
assert.throws(() => coll.getDB().watch({}, {apiDeprecationErrors: true}));

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

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const primaryPort = rst.getPrimary().port;

/*
 * Test that we can call replSetGetStatus while assembling the shell prompt, although
 * replSetGetStatus is not in API Version 1 and the shell is running with --apiStrict.
 */
const testPrompt = "assert(RegExp('PRIMARY').test(defaultPrompt()))";
const result = runMongoProgram(
    'mongo', '--port', primaryPort, '--apiVersion', '1', '--apiStrict', '--eval', testPrompt);
assert.eq(result, 0, `Error running shell with script '${testPrompt}'`);

/*
 * Test transaction-continuing commands with API parameters.
 */
m = new Mongo(`localhost:${primaryPort}`, undefined, {api: {version: '1'}});
for (let requireApiVersion of [false, true]) {
    // Omit api = {}, that's tested elsewhere.
    for (let api of [{version: '1'},
                     {version: '1', strict: true},
                     {version: '1', deprecationErrors: true},
                     {version: '1', strict: true, deprecationErrors: true},
    ]) {
        assert.commandWorked(
            m.getDB("admin").runCommand({setParameter: 1, requireApiVersion: requireApiVersion}));

        // Start a transaction with the right API version parameters. Use an explicit lsid to
        // override implicit session creation.
        const lsid = UUID();
        const versionedConn = new Mongo(`localhost:${primaryPort}`, undefined, {api: api});
        assert.commandWorked(versionedConn.getDB('admin').runCommand({
            find: 'collection',
            lsid: {id: lsid},
            txnNumber: NumberLong(1),
            autocommit: false,
            startTransaction: true
        }));

        const continueCmd =
            {find: 'collection', lsid: {id: lsid}, txnNumber: NumberLong(1), autocommit: false};

        runShellWithCommand(
            primaryPort, requireApiVersion, true /* expectSuccess */, continueCmd, api);
        newMongo(primaryPort, requireApiVersion, true /* expectSuccess */, continueCmd, api);
    }
}

assert.commandWorked(m.getDB('admin').runCommand({setParameter: 1, requireApiVersion: false}));

rst.stopSet();
})();
