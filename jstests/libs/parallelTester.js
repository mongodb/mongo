export var Thread;

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
