/**
 * The ParallelTester class is used to test more than one test concurrently
 */
if (typeof _threadInject != "undefined") {
    // With --enableJavaScriptProtection functions are presented as Code objects.
    // This function evals all the Code objects then calls the provided start function.
    // arguments: [startFunction, startFunction args...]
    function _threadStartWrapper() {
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
        for (var i = 0, l = arguments.length; i < l; i++) {
            newArgs.push(evalCodeArgs(arguments[i]));
        }
        realStartFn = newArgs.shift();
        return realStartFn.apply(this, newArgs);
    }

    Thread = function() {
        var args = Array.prototype.slice.call(arguments);
        args.unshift(_threadStartWrapper);
        this.init.apply(this, args);
    };
    _threadInject(Thread.prototype);

    ScopedThread = function() {
        var args = Array.prototype.slice.call(arguments);
        args.unshift(_threadStartWrapper);
        this.init.apply(this, args);
    };
    ScopedThread.prototype = new Thread(function() {});
    _scopedThreadInject(ScopedThread.prototype);

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
        var t = m.getDB("test")[collectionName];
        for (var i in args) {
            sleep(args[i][0]);
            eval(args[i][1]);
        }
    };

    // Helper class for running tests in parallel.  It assembles a set of tests
    // and then calls assert.parallelests to run them.
    ParallelTester = function() {
        assert.neq(db.getMongo().writeMode(), "legacy", "wrong shell write mode");
        this.params = new Array();
    };

    ParallelTester.prototype.add = function(fun, args) {
        args = args || [];
        args.unshift(fun);
        this.params.push(args);
    };

    ParallelTester.prototype.run = function(msg, newScopes) {
        newScopes = newScopes || false;
        assert.parallelTests(this.params, msg, newScopes);
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
            "repair.js",
            "cursor8.js",
            "recstore.js",
            "extent.js",
            "indexb.js",

            // Tests that set a parameter that causes the server to ignore
            // long index keys.
            "index_bigkeys_nofail.js",
            "index_bigkeys_validation.js",

            "mr_drop.js",
            "mr3.js",
            "indexh.js",
            "evald.js",
            "evalf.js",
            "killop.js",
            "run_program1.js",
            "notablescan.js",
            "drop2.js",
            "dropdb_race.js",
            "bench_test1.js",
            "padding.js",
            "queryoptimizera.js",
            "loglong.js",  // log might overflow before
            // this has a chance to see the message
            "connections_opened.js",  // counts connections, globally
            "opcounters_write_cmd.js",
            "currentop.js",                   // SERVER-8673, plus rwlock yielding issues
            "set_param1.js",                  // changes global state
            "geo_update_btree2.js",           // SERVER-11132 test disables table scans
            "update_setOnInsert.js",          // SERVER-9982
            "max_time_ms.js",                 // Sensitive to query execution time, by design
            "collection_info_cache_race.js",  // Requires collection exists

            // This overwrites MinKey/MaxKey's singleton which breaks
            // any other test that uses MinKey/MaxKey
            "type6.js",

            // Assumes that other tests are not creating cursors.
            "kill_cursors.js",
        ]);

        var parallelFilesDir = "jstests/core";

        // some tests can't be run in parallel with each other
        var serialTestsArr = [
            parallelFilesDir + "/fsync.js",
            parallelFilesDir + "/auth1.js",

            // These tests expect the profiler to be on or off at specific points. They should not
            // be run in parallel with tests that peform fsyncLock. User operations skip writing to
            // the system.profile collection while the server is fsyncLocked.
            //
            // The profiler tests can be run in parallel with each other as they use test-specific
            // databases.
            parallelFilesDir + "/apitest_db.js",
            parallelFilesDir + "/evalb.js",
            parallelFilesDir + "/geo_s2cursorlimitskip.js",
            parallelFilesDir + "/profile1.js",
            parallelFilesDir + "/profile2.js",
            parallelFilesDir + "/profile3.js",
            parallelFilesDir + "/profile_agg.js",
            parallelFilesDir + "/profile_count.js",
            parallelFilesDir + "/profile_delete.js",
            parallelFilesDir + "/profile_distinct.js",
            parallelFilesDir + "/profile_find.js",
            parallelFilesDir + "/profile_findandmodify.js",
            parallelFilesDir + "/profile_geonear.js",
            parallelFilesDir + "/profile_getmore.js",
            parallelFilesDir + "/profile_group.js",
            parallelFilesDir + "/profile_insert.js",
            parallelFilesDir + "/profile_mapreduce.js",
            parallelFilesDir + "/profile_no_such_db.js",
            parallelFilesDir + "/profile_update.js"
        ];
        var serialTests = makeKeys(serialTestsArr);

        // prefix the first thread with the serialTests
        // (which we will exclude from the rest of the threads below)
        params[0] = serialTestsArr;
        var files = listFiles(parallelFilesDir);
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
                load(x);
            }, 1);
            print("         S" + suite + " Test : " + x + " " + time + "ms");
        });
    };

    // params: array of arrays, each element of which consists of a function followed
    // by zero or more arguments to that function.  Each function and its arguments will
    // be called in a separate thread.
    // msg: failure message
    // newScopes: if true, each thread starts in a fresh scope
    assert.parallelTests = function(params, msg, newScopes) {
        newScopes = newScopes || false;
        var wrapper = function(fun, argv) {
            eval("var z = function() {" + "TestData = " + tojson(TestData) + ";" +
                 "var __parallelTests__fun = " + fun.toString() + ";" +
                 "var __parallelTests__argv = " + tojson(argv) + ";" +
                 "var __parallelTests__passed = false;" + "try {" +
                 "__parallelTests__fun.apply( 0, __parallelTests__argv );" +
                 "__parallelTests__passed = true;" + "} catch ( e ) {" + "print('');" +
                 "print( '********** Parallel Test FAILED: ' + tojson(e) );" + "print('');" + "}" +
                 "return __parallelTests__passed;" + "}");
            return z;
        };
        var runners = new Array();
        for (var i in params) {
            var param = params[i];
            var test = param.shift();
            var t;
            if (newScopes)
                t = new ScopedThread(wrapper(test, param));
            else
                t = new Thread(wrapper(test, param));
            runners.push(t);
        }

        runners.forEach(function(x) {
            x.start();
        });
        var nFailed = 0;
        // SpiderMonkey doesn't like it if we exit before all threads are joined
        // (see SERVER-19615 for a similar issue).
        runners.forEach(function(x) {
            if (!x.returnData()) {
                ++nFailed;
            }
        });
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
