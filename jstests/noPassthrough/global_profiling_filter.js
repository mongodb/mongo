/*
 * Test the usage and behavior of the 'setProfilingFilterGlobally' command.
 *
 * @tags: [requires_sharding, requires_replication]
 */
(function() {

load("jstests/libs/log.js");                                     // For findMatchingLogLine.
load("jstests/libs/fixture_helpers.js");                         // For FixtureHelpers.
load("jstests/noPassthrough/libs/server_parameter_helpers.js");  // For setParameter.

// Updates the global profiling filter to 'newFilter' and validates that 'oldFilter' is returned in
// the response and the change is logged correctly. If `newFilter' is null, unsets the filter.
function updateProfilingFilterGlobally(db, {oldFilter, newFilter}) {
    const result = assert.commandWorked(
        db.runCommand({setProfilingFilterGlobally: 1, filter: newFilter ? newFilter : "unset"}));
    assert.eq(result.was, oldFilter ? oldFilter : "none");

    const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
    assert(!!findMatchingLogLine(log, {
        msg: "Profiler settings changed globally",
        from: {filter: oldFilter ? oldFilter : "none"},
        to: {filter: newFilter ? newFilter : "none"}
    }),
           "expected log line was not found");
}

(function testQueryKnobMustBeEnabledToUseCommand() {
    // First make sure we can't run the command with the query knob turned off.
    const conn = MongoRunner.runMongod({});
    assert.commandFailedWithCode(
        conn.adminCommand({setProfilingFilterGlobally: 1, filter: "unset"}), 7283301);
    MongoRunner.stopMongod(conn);
})();

(function testCommandOverridesDefaultFromConfigFile() {
    // Test that setting the global filter overrides the startup configuration.
    const conn = MongoRunner.runMongod({
        config: "jstests/libs/config_files/set_profiling_filter.json",
        setParameter: {internalQueryGlobalProfilingFilter: 1}
    });
    updateProfilingFilterGlobally(conn.getDB("test"), {
        oldFilter: {$expr: {$lt: [{$rand: {}}, {$const: 0.01}]}},
        newFilter: {$expr: {$lt: [{$rand: {}}, {$const: 0.5}]}}
    });
    updateProfilingFilterGlobally(
        conn.getDB("test"),
        {oldFilter: {$expr: {$lt: [{$rand: {}}, {$const: 0.5}]}}, newFilter: null});
    MongoRunner.stopMongod(conn);
})();

// Run a set of correctness tests for the setProfilingFilterGlobally command on the given
// connection.
function runCorrectnessTests(conn) {
    // mongoS supports slow-query log lines but not profiling. So if we are talking to mongoS, we
    // will avoid enabling profiling, or making assertions about profiling.
    const db = conn.getDB("test");
    const isMongos = FixtureHelpers.isMongos(db);

    const dbs = [conn.getDB("db1"), conn.getDB("db2"), conn.getDB("db3")];

    function initDatabase(db) {
        db.c.drop();
        db.c.insert([{x: 25}, {x: 50}]);

        // Fully enable profiling to start.
        db.setProfilingLevel(isMongos ? 0 : 1, {slowms: -1});
    }

    // Initialize each database with some basic data and set the profiling level.
    for (const db of dbs) {
        initDatabase(db);
    }

    // Test queries to run.
    const queries = {
        query1: {filter: {}, nreturned: 2},
        query2: {filter: {x: 25}, nreturned: 1},
        query3: {filter: {x: 1000}, nreturned: 0},
    };

    // Profile filters matching specific test queries.
    const profileFilter1 = {filter: {nreturned: {$eq: 2}}, matchesQuery: "query1"};
    const profileFilter2 = {filter: {nreturned: {$eq: 1}}, matchesQuery: "query2"};

    // Verify that profile filters are applied as expected for both logging and profiling, if
    // applicable. 'params' must specify three fields:
    //      'desc': short test description
    //      'globalFilter': expected global profiling filter setting
    //      'dbFilters': expected database-specific filter settings
    function verify(params) {
        function verifyDatabase(db) {
            // Create a unique comment for the query.
            function queryComment(queryName) {
                return `${params.desc}: ${db.getName()} -> ${queryName}`;
            }

            // Check the log for the query's profile entry, and system.profile if this is being run
            // on mongod.
            function assertQueryProfiled(name, shouldProfile) {
                const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
                assert.eq(
                    !!findMatchingLogLine(log, {msg: "Slow query", comment: queryComment(name)}),
                    shouldProfile,
                    `expected query${shouldProfile ? "" : " not"} to be logged: ${
                        queryComment(name)}`);

                if (!isMongos) {
                    assert.eq(!!db.system.profile.findOne({'command.comment': queryComment(name)}),
                              shouldProfile,
                              `expected query${shouldProfile ? "" : " not"} to be profiled: ${
                                  queryComment(name)}`);
                }
            }

            for (const [queryName, query] of Object.entries(queries)) {
                // First, run the query on the database.
                const comment = queryComment(queryName);
                const results = db.c.find(query.filter).comment(comment).itcount();
                assert.eq(results, query.nreturned, comment);

                // Validate whether or not the query was profiled based on the database's
                // profile filter. We should profile *unless* there is a filter applicable to
                // this database that does not match the query.
                let shouldProfile = true;
                if (params.dbFilters.hasOwnProperty(db.getName())) {
                    // This database has a database specific filter, does it match the query?
                    shouldProfile = params.dbFilters[db.getName()].matchesQuery === queryName;
                } else if (params.globalFilter) {
                    // No database filter, but there is a global filter, does it match the
                    // query?
                    shouldProfile = params.globalFilter.matchesQuery == queryName;
                }
                assertQueryProfiled(queryName, shouldProfile);
            }
        }

        for (const db of dbs) {
            verifyDatabase(db);
        }

        // Now create a new database and make sure the global settings are applied correctly.
        const newDB = conn.getDB("newDB");
        initDatabase(newDB);
        verifyDatabase(newDB);
        newDB.dropDatabase();
    }

    // Error cases (invalid filters or parameters).
    assert.commandFailedWithCode(db.runCommand({setProfilingFilterGlobally: 1, filter: null}),
                                 ErrorCodes.BadValue);
    assert.commandFailedWithCode(
        db.runCommand({setProfilingFilterGlobally: 1, filter: {noSuchField: 1}}), 4910200);
    assert.commandFailedWithCode(
        db.runCommand({setProfilingFilterGlobally: 1, filter: {}, writeConcern: {w: "majority"}}),
        ErrorCodes.InvalidOptions);
    assert.commandFailedWithCode(
        db.runCommand(
            {setProfilingFilterGlobally: 1, filter: {}, readConcern: {level: "majority"}}),
        ErrorCodes.InvalidOptions);

    // Make sure that behavior is as expected in the default/startup state.
    (function testDefaultSettingIsNone() {
        verify({desc: "default setting is none", globalFilter: null, dbFilters: {}});
    })();

    (function testEmptyFilter() {
        updateProfilingFilterGlobally(db, {oldFilter: null, newFilter: {}});
        // Empty filter is always true - effectively none.
        verify({desc: "empty filter", globalFilter: null, dbFilters: {}});
    })();

    (function testGlobalFilterSettingAffectsAllDatabases() {
        updateProfilingFilterGlobally(db, {oldFilter: {}, newFilter: profileFilter1.filter});
        verify({
            desc: "setting the global filter affects all databases",
            globalFilter: profileFilter1,
            dbFilters: {}
        });
    })();

    (function testGlobalFilterUnsetClearsGlobalSetting() {
        updateProfilingFilterGlobally(db, {oldFilter: profileFilter1.filter, newFilter: null});
        verify({
            desc: "unsetting the global filter clears the global setting",
            globalFilter: null,
            dbFilters: {}
        });
    })();

    (function testGlobalFilterUnsetWhenAlreadyUnsetIsNoop() {
        updateProfilingFilterGlobally(db, {oldFilter: null, newFilter: null});
        verify({
            desc: "unsetting the global filter when already unset is a noop",
            globalFilter: null,
            dbFilters: {}
        });
    })();

    (function testGlobalFilterSettingOverridesDatabaseSpecificSettings() {
        let result = assert.commandWorked(db.getSiblingDB("db1").runCommand(
            {profile: isMongos ? 0 : 1, filter: profileFilter1.filter}));
        assert(!result.filter);
        result = assert.commandWorked(db.getSiblingDB("db2").runCommand(
            {profile: isMongos ? 0 : 1, filter: profileFilter2.filter}));
        assert(!result.filter);
        verify({
            desc: "setting the global filter overrides database specific settings (pre-validate)",
            globalFilter: null,
            dbFilters: {db1: profileFilter1, db2: profileFilter2}
        });

        updateProfilingFilterGlobally(db, {oldFilter: null, newFilter: profileFilter2.filter});
        verify({
            desc: "setting the global filter overrides database specific settings",
            globalFilter: profileFilter2,
            dbFilters: {}
        });
    })();

    (function testGlobalFilterSettingOverridesDatabaseSpecificSettingsEvenWhenNoop() {
        let result = assert.commandWorked(db.getSiblingDB("db1").runCommand(
            {profile: isMongos ? 0 : 1, filter: profileFilter1.filter}));
        assert.eq(result.filter, profileFilter2.filter);
        verify({
            desc:
                "setting the global filter overrides database specific settings even when a noop (pre-validate)",
            globalFilter: profileFilter2,
            dbFilters: {db1: profileFilter1}
        });

        updateProfilingFilterGlobally(
            db, {oldFilter: profileFilter2.filter, newFilter: profileFilter2.filter});
        verify({
            desc: "setting the global filter overrides database specific settings even when a noop",
            globalFilter: profileFilter2,
            dbFilters: {}
        });
    })();

    (function testGlobalFilterUnsetOverridesDatabaseSpecificSettings() {
        result = assert.commandWorked(db.getSiblingDB("db1").runCommand(
            {profile: isMongos ? 0 : 1, filter: profileFilter1.filter}));
        assert.eq(result.filter, profileFilter2.filter);
        result = assert.commandWorked(db.getSiblingDB("db3").runCommand(
            {profile: isMongos ? 0 : 1, filter: profileFilter2.filter}));
        assert.eq(result.filter, profileFilter2.filter);
        verify({
            desc: "unsetting the global filter overrides database specific settings (pre-validate)",
            globalFilter: profileFilter2,
            dbFilters: {db1: profileFilter1, db3: profileFilter2}
        });

        updateProfilingFilterGlobally(db, {oldFilter: profileFilter2.filter, newFilter: null});
        verify({
            desc: "unsetting the global filter overrides database specific settings",
            globalFilter: null,
            dbFilters: {}
        });
    })();
}

{
    // Run tests on mongod.
    const conn = MongoRunner.runMongod({setParameter: {internalQueryGlobalProfilingFilter: 1}});
    runCorrectnessTests(conn);
    MongoRunner.stopMongod(conn);
}

{
    // Run tests on mongos.
    const st = ShardingTest({
        shards: 1,
        rs: {nodes: 1},
        config: 1,
        mongosOptions: {setParameter: {internalQueryGlobalProfilingFilter: 1}}
    });
    runCorrectnessTests(st);
    st.stop();
}
})();
