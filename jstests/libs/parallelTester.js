/**
 * The ParallelTester class is used to test more than one test concurrently
 */

export var Thread, fork, EventGenerator, ParallelTester;

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
            } else if (arg !== null && isObject(arg) && !(arg instanceof Date)) {
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
        if (TestData && TestData["shellGRPC"]) {
            eval('import("jstests/libs/override_methods/enable_grpc_on_connect.js")');
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

    ParallelTester.prototype.run = async function(msg) {
        await assert.parallelTests(this.params, msg);
    };

    async function measureAsync(fn) {
        const start = new Date();
        await fn.apply(null, Array.from(arguments).slice(2));
        return (new Date()).getTime() - start.getTime();
    }

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

globalThis.CountDownLatch = Object.extend(function(count) {
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
