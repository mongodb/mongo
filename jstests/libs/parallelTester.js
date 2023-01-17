/**
 * The ParallelTester class is used to test more than one test concurrently
 */
if (typeof _threadInject != "undefined") {
    // With --enableJavaScriptProtection functions are presented as Code objects.
    // This function evals all the Code objects then calls the provided start function.
    // arguments: [startFunction, startFunction args...]
    function _threadStartWrapper(testData) {
        // Recursively evals all the Code objects present in arguments
        // NOTE: This is a naive implementation that cannot handle cyclic objects.
        function evalCodeArgs(arg) {
            if (arg instanceof Code) {
                return eval("(" + arg.code + ")");
            } else if (arg !== null && isObject(arg)) {
                var newArg = arg instanceof Array ? [] : {};
                for (var prop in arg) {
                    if (arg.hasOwnProperty(prop)) {
                        newArg[prop] = evalCodeArgs(arg[prop]);
                    }
                }
                return newArg;
            }
            return arg;
        }
        var realStartFn;
        var newArgs = [];
        // We skip the first argument, which is always TestData.
        TestData = evalCodeArgs(testData);
        for (var i = 1, l = arguments.length; i < l; i++) {
            newArgs.push(evalCodeArgs(arguments[i]));
        }
        realStartFn = newArgs.shift();
        return realStartFn.apply(this, newArgs);
    }

    Thread = function() {
        var args = Array.prototype.slice.call(arguments);
        // Always pass TestData as the first argument.
        args.unshift(TestData);
        args.unshift(_threadStartWrapper);
        this.init.apply(this, args);
    };
    _threadInject(Thread.prototype);

    fork = function() {
        var t = new Thread(function() {});
        Thread.apply(t, arguments);
        return t;
    };

    // Helper class to generate a list of events which may be executed by a ParallelTester
    EventGenerator = function(me, collectionName, mean, host) {
        this.mean = mean;
        if (host == undefined)
            host = db.getMongo().host;
        this.events = new Array(me, collectionName, host);
    };

    EventGenerator.prototype._add = function(action) {
        this.events.push([Random.genExp(this.mean), action]);
    };

    EventGenerator.prototype.addInsert = function(obj) {
        this._add("t.insert( " + tojson(obj) + " )");
    };

    EventGenerator.prototype.addRemove = function(obj) {
        this._add("t.remove( " + tojson(obj) + " )");
    };

    EventGenerator.prototype.addCurrentOp = function() {
        this._add("db.currentOp()");
    };

    EventGenerator.prototype.addUpdate = function(objOld, objNew) {
        this._add("t.update( " + tojson(objOld) + ", " + tojson(objNew) + " )");
    };

    EventGenerator.prototype.addCheckCount = function(count, query, shouldPrint, checkQuery) {
        query = query || {};
        shouldPrint = shouldPrint || false;
        checkQuery = checkQuery || false;
        var action = "assert.eq( " + count + ", t.count( " + tojson(query) + " ) );";
        if (checkQuery) {
            action +=
                " assert.eq( " + count + ", t.find( " + tojson(query) + " ).toArray().length );";
        }
        if (shouldPrint) {
            action += " print( me + ' ' + " + count + " );";
        }
        this._add(action);
    };

    EventGenerator.prototype.getEvents = function() {
        return this.events;
    };

    EventGenerator.dispatch = function() {
        var args = Array.from(arguments);
        var me = args.shift();
        var collectionName = args.shift();
        var host = args.shift();
        var m = new Mongo(host);

        // We define 'db' and 't' as local variables so that calling eval() on the stringified
        // JavaScript expression 'args[i][1]' can take advantage of using them.
        var db = m.getDB("test");
        var t = db[collectionName];
        for (var i in args) {
            sleep(args[i][0]);
            eval(args[i][1]);
        }
    };

    // Helper class for running tests in parallel.  It assembles a set of tests
    // and then calls assert.parallelests to run them.
    ParallelTester = function() {
        this.params = new Array();
    };

    ParallelTester.prototype.add = function(fun, args) {
        args = args || [];
        args.unshift(fun);
        this.params.push(args);
    };

    ParallelTester.prototype.run = function(msg) {
        assert.parallelTests(this.params, msg);
    };

    // creates lists of tests from jstests dir in a format suitable for use by
    // ParallelTester.fileTester.  The lists will be in random order.
    // n: number of lists to split these tests into
    ParallelTester.createJstestsLists = function(n) {
        var params = new Array();
        for (var i = 0; i < n; ++i) {
            params.push([]);
        }

        var makeKeys = function(a) {
            var ret = {};
            for (var i in a) {
                ret[a[i]] = 1;
            }
            return ret;
        };

        // some tests can't run in parallel with most others
        var skipTests = makeKeys([
            "index/indexb.js",

            // Tests that set a parameter that causes the server to ignore
            // long index keys.
            "index_bigkeys_nofail.js",
            "index_bigkeys_validation.js",

            // Tests that set the notablescan parameter, which makes queries fail rather than use a
            // non-indexed plan.
            "notablescan.js",
            "notablescan_capped.js",

            "query/mr/mr_fail_invalid_js.js",
            "run_program1.js",
            "bench_test1.js",

            // These tests use getLog to examine the logs. Tests which do so shouldn't be run in
            // this suite because any test being run at the same time could conceivably spam the
            // logs so much that the line they are looking for has been rotated off the server's
            // in-memory buffer of log messages, which only stores the 1024 most recent operations.
            "comment_field.js",
            "administrative/getlog2.js",
            "logprocessdetails.js",
            "query/queryoptimizera.js",
            "log_remote_op_wait.js",

            "connections_opened.js",  // counts connections, globally
            "opcounters_write_cmd.js",
            "administrative/set_param1.js",        // changes global state
            "index/geo/geo_update_btree2.js",      // SERVER-11132 test disables table scans
            "write/update/update_setOnInsert.js",  // SERVER-9982
            "max_time_ms.js",                      // Sensitive to query execution time, by design
            "shell/autocomplete.js",               // Likewise.

            // This overwrites MinKey/MaxKey's singleton which breaks
            // any other test that uses MinKey/MaxKey
            "query/type/type6.js",

            // Assumes that other tests are not creating cursors.
            "kill_cursors.js",

            // Assumes that other tests are not starting operations.
            "administrative/current_op/currentop_shell.js",

            // These tests check global command counters.
            "write/find_and_modify/find_and_modify_metrics.js",
            "write/update/update_metrics.js",

            // Views tests
            "views/invalid_system_views.js",      // Puts invalid view definitions in system.views.
            "views/views_all_commands.js",        // Drops test DB.
            "views/view_with_invalid_dbname.js",  // Puts invalid view definitions in system.views.

            // This test causes collMod commands to hang, which interferes with other tests running
            // collMod.
            "write/crud_ops_do_not_throw_locktimeout.js",

            // Can fail if isMaster takes too long on a loaded machine.
            "dbadmin.js",

            // Other tests will fail while the requireApiVersion server parameter is set.
            "require_api_version.js",

            // This sets the 'disablePipelineOptimization' failpoint, which causes other tests
            // running in parallel to fail if they were expecting their pipelines to be optimized.
            "type_bracket.js",

            // This test updates global memory usage counters in the bucket catalog in a way that
            // may affect other time-series tests running concurrently.
            "timeseries/timeseries_idle_buckets.js",

            // Assumes that other tests are not creating API version 1 incompatible data.
            "administrative/validate_db_metadata_command.js",

            // The tests in 'bench_test*.js' files use 'benchRun()'. The main purpose of
            // 'benchRun()' is for performance testing and the 'benchRun()' implementation itself
            // launches multiple threads internally, it's not necessary to keep 'bench_test*.js'
            // within the parallel test job.
            "bench_test1.js",
            "bench_test2.js",

            // These tests cause deletes and updates to hang, which may affect other tests running
            // concurrently.
            "timeseries/timeseries_delete_hint.js",
            "timeseries/timeseries_update_hint.js",
            "timeseries/timeseries_delete_concurrent.js",
            "timeseries/timeseries_update_concurrent.js",

            // These tests rely on no writes happening that would force oplog truncation.
            "write_change_stream_pit_preimage_in_transaction.js",
            "write/write_change_stream_pit_preimage.js",

            // These tests convert a non-unique index to a unique one, which is not compatible
            // when running against inMemory storage engine variants. Since this test only fails
            // in the parallel tester, which does not respect test tags, we omit the tests
            // instead of manually checking TestData values in the mongo shell for the Evergreen
            // variant.
            "ddl/collmod_convert_index_uniqueness.js",
            "ddl/collmod_convert_to_unique_apply_ops.js",
            "ddl/collmod_convert_to_unique_violations.js",
            "ddl/collmod_convert_to_unique_violations_size_limit.js",

            // The parallel tester does not respect test tags, compact cannot run against the
            // inMemory storage engine.
            "timeseries/timeseries_compact.js",

            // TODO (SERVER-63228): Remove this exclusion once the feature flag is enabled by
            // default.
            "timeseries/timeseries_index_ttl_partial.js",
        ]);

        // Get files, including files in subdirectories.
        var getFilesRecursive = function(dir) {
            var files = listFiles(dir);
            var fileList = [];
            files.forEach(file => {
                if (file.isDirectory) {
                    getFilesRecursive(file.name).forEach(subDirFile => fileList.push(subDirFile));
                } else {
                    fileList.push(file);
                }
            });
            return fileList;
        };

        // Transactions are not supported on standalone nodes so we do not run them here.
        // NOTE: We need to take substring of the full test path to ensure that 'jstests/core/' is
        // not included.
        const txnsTestFiles =
            getFilesRecursive("jstests/core/txns/")
                .map(fullPathToTest => fullPathToTest.name.substring("jstests/core/".length));
        Object.assign(skipTests, makeKeys(txnsTestFiles));

        var parallelFilesDir = "jstests/core";

        // some tests can't be run in parallel with each other
        var serialTestsArr = [
            // These tests use fsyncLock.
            parallelFilesDir + "/fsync.js",
            parallelFilesDir + "/administrative/current_op/currentop.js",
            parallelFilesDir + "/ddl/killop_drop_collection.js",

            // These tests expect the profiler to be on or off at specific points. They should not
            // be run in parallel with tests that perform fsyncLock. User operations skip writing to
            // the system.profile collection while the server is fsyncLocked.
            //
            // Most profiler tests can be run in parallel with each other as they use test-specific
            // databases, with the exception of tests which modify slowms or the profiler's sampling
            // rate, since those affect profile settings globally.
            parallelFilesDir + "/api/apitest_db_profile_level.js",
            parallelFilesDir + "/index/geo/geo_s2cursorlimitskip.js",
            parallelFilesDir + "/administrative/profile/profile1.js",
            parallelFilesDir + "/administrative/profile/profile2.js",
            parallelFilesDir + "/administrative/profile/profile3.js",
            parallelFilesDir + "/administrative/profile/profile_agg.js",
            parallelFilesDir + "/administrative/profile/profile_count.js",
            parallelFilesDir + "/administrative/profile/profile_delete.js",
            parallelFilesDir + "/administrative/profile/profile_distinct.js",
            parallelFilesDir + "/administrative/profile/profile_find.js",
            parallelFilesDir + "/administrative/profile/profile_findandmodify.js",
            parallelFilesDir + "/administrative/profile/profile_getmore.js",
            parallelFilesDir + "/administrative/profile/profile_hide_index.js",
            parallelFilesDir + "/administrative/profile/profile_insert.js",
            parallelFilesDir + "/administrative/profile/profile_list_collections.js",
            parallelFilesDir + "/administrative/profile/profile_list_indexes.js",
            parallelFilesDir + "/administrative/profile/profile_mapreduce.js",
            parallelFilesDir + "/administrative/profile/profile_no_such_db.js",
            parallelFilesDir + "/administrative/profile/profile_query_hash.js",
            parallelFilesDir + "/administrative/profile/profile_sampling.js",
            parallelFilesDir + "/administrative/profile/profile_update.js",
            parallelFilesDir + "/query/plan_cache/cached_plan_trial_does_not_discard_work.js",
            parallelFilesDir + "/sbe/from_plan_cache_flag.js",
            parallelFilesDir + "/timeseries/bucket_unpacking_with_sort_plan_cache.js",

            // These tests rely on a deterministically refreshable logical session cache. If they
            // run in parallel, they could interfere with the cache and cause failures.
            parallelFilesDir + "/administrative/list_all_local_sessions.js",
            parallelFilesDir + "/administrative/list_all_sessions.js",
            parallelFilesDir + "/administrative/list_sessions.js",
        ];
        var serialTests = makeKeys(serialTestsArr);

        // prefix the first thread with the serialTests
        // (which we will exclude from the rest of the threads below)
        params[0] = serialTestsArr;
        var files = getFilesRecursive(parallelFilesDir);
        files = Array.shuffle(files);

        var i = 0;
        files.forEach(function(x) {
            if ((/[\/\\]_/.test(x.name)) || (!/\.js$/.test(x.name)) ||
                (x.name.match(parallelFilesDir + "/(.*\.js)")[1] in skipTests) ||  //
                (x.name in serialTests)) {
                print(" >>>>>>>>>>>>>>> skipping " + x.name);
                return;
            }
            // add the test to run in one of the threads.
            params[i % n].push(x.name);
            ++i;
        });

        // randomize ordering of the serialTests
        params[0] = Array.shuffle(params[0]);

        for (var i in params) {
            params[i].unshift(i);
        }

        return params;
    };

    // runs a set of test files
    // first argument is an identifier for this tester, remaining arguments are file names
    ParallelTester.fileTester = function() {
        var args = Array.from(arguments);
        var suite = args.shift();
        args.forEach(function(x) {
            print("         S" + suite + " Test : " + x + " ...");
            var time = Date.timeFunc(function() {
                // Create a new connection to the db for each file. If tests share the same
                // connection it can create difficult to debug issues.
                db = new Mongo(db.getMongo().host).getDB(db.getName());
                gc();
                load(x);
            }, 1);
            print("         S" + suite + " Test : " + x + " " + time + "ms");
        });
    };

    // params: array of arrays, each element of which consists of a function followed
    // by zero or more arguments to that function.  Each function and its arguments will
    // be called in a separate thread.
    // msg: failure message
    assert.parallelTests = function(params, msg) {
        function wrapper(fun, argv, globals) {
            if (globals.hasOwnProperty("TestData")) {
                TestData = globals.TestData;
            }

            try {
                fun.apply(0, argv);
                return {passed: true};
            } catch (e) {
                print("\n********** Parallel Test FAILED: " + tojson(e) + "\n");
                return {
                    passed: false,
                    testName: tojson(e).match(/Error: error loading js file: (.*\.js)/)[1]
                };
            }
        }

        TestData.isParallelTest = true;

        var runners = new Array();
        for (var i in params) {
            var param = params[i];
            var test = param.shift();

            // Make a shallow copy of TestData so we can override the test name to
            // prevent tests on different threads that to use jsTestName() as the
            // collection name from colliding.
            const clonedTestData = Object.assign({}, TestData);
            clonedTestData.testName = `ParallelTesterThread${i}`;
            var t = new Thread(wrapper, test, param, {TestData: clonedTestData});
            runners.push(t);
        }

        runners.forEach(function(x) {
            x.start();
        });
        var nFailed = 0;
        var failedTests = [];
        // SpiderMonkey doesn't like it if we exit before all threads are joined
        // (see SERVER-19615 for a similar issue).
        runners.forEach(function(x) {
            if (!x.returnData().passed) {
                ++nFailed;
                failedTests.push(x.returnData().testName);
            }
        });
        msg += ": " + tojsononeline(failedTests);
        assert.eq(0, nFailed, msg);
    };
}

if (typeof CountDownLatch !== 'undefined') {
    CountDownLatch = Object.extend(function(count) {
        if (!(this instanceof CountDownLatch)) {
            return new CountDownLatch(count);
        }
        this._descriptor = CountDownLatch._new.apply(null, arguments);

        // NOTE: The following methods have to be defined on the instance itself,
        //       and not on its prototype. This is because properties on the
        //       prototype are lost during the serialization to BSON that occurs
        //       when passing data to a child thread.

        this.await = function() {
            CountDownLatch._await(this._descriptor);
        };
        this.countDown = function() {
            CountDownLatch._countDown(this._descriptor);
        };
        this.getCount = function() {
            return CountDownLatch._getCount(this._descriptor);
        };
    }, CountDownLatch);
}
