/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES7 draft (January 21, 2016) 19.2.3.2 Function.prototype.bind
function FunctionBind(thisArg, ...boundArgs) {
    // Step 1.
    var target = this;
    // Step 2.
    if (!IsCallable(target))
        ThrowTypeError(JSMSG_INCOMPATIBLE_PROTO, "Function", "bind", target);

    // Step 3 (implicit).
    // Step 4.
    var F;
    var argCount = boundArgs.length;
    switch (argCount) {
      case 0:
        F = bind_bindFunction0(target, thisArg, boundArgs);
        break;
      case 1:
        F = bind_bindFunction1(target, thisArg, boundArgs);
        break;
      case 2:
        F = bind_bindFunction2(target, thisArg, boundArgs);
        break;
      default:
        F = bind_bindFunctionN(target, thisArg, boundArgs);
    }

    // Steps 5-11.
    _FinishBoundFunctionInit(F, target, argCount);

    // Ensure that the apply intrinsic has been cloned so it can be baked into
    // JIT code.
    void std_Function_apply;

    // Step 12.
    return F;
}
/**
 * bind_bindFunction{0,1,2} are special cases of the generic bind_bindFunctionN
 * below. They avoid the need to merge the lists of bound arguments and call
 * arguments to the bound function into a new list which is then used in a
 * destructuring call of the bound function.
 *
 * All three of these functions again have special-cases for call argument
 * counts between 0 and 5. For calls with 6+ arguments, all - bound and call -
 * arguments are copied into an array before invoking the generic call and
 * construct helper functions. This avoids having to use rest parameters and
 * destructuring in the fast path.
 *
 * Directly embedding the for-loop to combine bound and call arguments may
 * inhibit inlining of the bound function, so we use a separate combiner
 * function to perform this task. This combiner function is created lazily to
 * ensure we only pay its construction cost when needed.
 *
 * All bind_bindFunction{X} functions have the same signature to enable simple
 * reading out of closed-over state by debugging functions.
 */
function bind_bindFunction0(fun, thisArg, boundArgs) {
    return function bound() {
        // Ensure we allocate a call-object slot for |boundArgs|, so the
        // debugger can access this value.
        if (false) void boundArgs;

        var newTarget;
        if (_IsConstructing()) {
            newTarget = new.target;
            if (newTarget === bound)
                newTarget = fun;
            switch (arguments.length) {
              case 0:
                return constructContentFunction(fun, newTarget);
              case 1:
                return constructContentFunction(fun, newTarget, SPREAD(arguments, 1));
              case 2:
                return constructContentFunction(fun, newTarget, SPREAD(arguments, 2));
              case 3:
                return constructContentFunction(fun, newTarget, SPREAD(arguments, 3));
              case 4:
                return constructContentFunction(fun, newTarget, SPREAD(arguments, 4));
              case 5:
                return constructContentFunction(fun, newTarget, SPREAD(arguments, 5));
              default:
                var args = FUN_APPLY(bind_mapArguments, null, arguments);
                return bind_constructFunctionN(fun, newTarget, args);
            }
        } else {
            switch (arguments.length) {
              case 0:
                return callContentFunction(fun, thisArg);
              case 1:
                return callContentFunction(fun, thisArg, SPREAD(arguments, 1));
              case 2:
                return callContentFunction(fun, thisArg, SPREAD(arguments, 2));
              case 3:
                return callContentFunction(fun, thisArg, SPREAD(arguments, 3));
              case 4:
                return callContentFunction(fun, thisArg, SPREAD(arguments, 4));
              case 5:
                return callContentFunction(fun, thisArg, SPREAD(arguments, 5));
              default:
                return FUN_APPLY(fun, thisArg, arguments);
            }
        }
    };
}

function bind_bindFunction1(fun, thisArg, boundArgs) {
    var bound1 = boundArgs[0];
    var combiner = null;
    return function bound() {
        // Ensure we allocate a call-object slot for |boundArgs|, so the
        // debugger can access this value.
        if (false) void boundArgs;

        var newTarget;
        if (_IsConstructing()) {
            newTarget = new.target;
            if (newTarget === bound)
                newTarget = fun;
            switch (arguments.length) {
              case 0:
                return constructContentFunction(fun, newTarget, bound1);
              case 1:
                return constructContentFunction(fun, newTarget, bound1, SPREAD(arguments, 1));
              case 2:
                return constructContentFunction(fun, newTarget, bound1, SPREAD(arguments, 2));
              case 3:
                return constructContentFunction(fun, newTarget, bound1, SPREAD(arguments, 3));
              case 4:
                return constructContentFunction(fun, newTarget, bound1, SPREAD(arguments, 4));
              case 5:
                return constructContentFunction(fun, newTarget, bound1, SPREAD(arguments, 5));
            }
        } else {
            switch (arguments.length) {
              case 0:
                return callContentFunction(fun, thisArg, bound1);
              case 1:
                return callContentFunction(fun, thisArg, bound1, SPREAD(arguments, 1));
              case 2:
                return callContentFunction(fun, thisArg, bound1, SPREAD(arguments, 2));
              case 3:
                return callContentFunction(fun, thisArg, bound1, SPREAD(arguments, 3));
              case 4:
                return callContentFunction(fun, thisArg, bound1, SPREAD(arguments, 4));
              case 5:
                return callContentFunction(fun, thisArg, bound1, SPREAD(arguments, 5));
            }
        }

        if (combiner === null) {
            combiner = function() {
                var callArgsCount = arguments.length;
                var args = std_Array(1 + callArgsCount);
                _DefineDataProperty(args, 0, bound1);
                for (var i = 0; i < callArgsCount; i++)
                    _DefineDataProperty(args, i + 1, arguments[i]);
                return args;
            };
        }

        var args = FUN_APPLY(combiner, null, arguments);
        if (newTarget === undefined)
            return bind_applyFunctionN(fun, thisArg, args);
        return bind_constructFunctionN(fun, newTarget, args);
    };
}

function bind_bindFunction2(fun, thisArg, boundArgs) {
    var bound1 = boundArgs[0];
    var bound2 = boundArgs[1];
    var combiner = null;
    return function bound() {
        // Ensure we allocate a call-object slot for |boundArgs|, so the
        // debugger can access this value.
        if (false) void boundArgs;

        var newTarget;
        if (_IsConstructing()) {
            newTarget = new.target;
            if (newTarget === bound)
                newTarget = fun;
            switch (arguments.length) {
              case 0:
                return constructContentFunction(fun, newTarget, bound1, bound2);
              case 1:
                return constructContentFunction(fun, newTarget, bound1, bound2, SPREAD(arguments, 1));
              case 2:
                return constructContentFunction(fun, newTarget, bound1, bound2, SPREAD(arguments, 2));
              case 3:
                return constructContentFunction(fun, newTarget, bound1, bound2, SPREAD(arguments, 3));
              case 4:
                return constructContentFunction(fun, newTarget, bound1, bound2, SPREAD(arguments, 4));
              case 5:
                return constructContentFunction(fun, newTarget, bound1, bound2, SPREAD(arguments, 5));
            }
        } else {
            switch (arguments.length) {
              case 0:
                return callContentFunction(fun, thisArg, bound1, bound2);
              case 1:
                return callContentFunction(fun, thisArg, bound1, bound2, SPREAD(arguments, 1));
              case 2:
                return callContentFunction(fun, thisArg, bound1, bound2, SPREAD(arguments, 2));
              case 3:
                return callContentFunction(fun, thisArg, bound1, bound2, SPREAD(arguments, 3));
              case 4:
                return callContentFunction(fun, thisArg, bound1, bound2, SPREAD(arguments, 4));
              case 5:
                return callContentFunction(fun, thisArg, bound1, bound2, SPREAD(arguments, 5));
            }
        }

        if (combiner === null) {
            combiner = function() {
                var callArgsCount = arguments.length;
                var args = std_Array(2 + callArgsCount);
                _DefineDataProperty(args, 0, bound1);
                _DefineDataProperty(args, 1, bound2);
                for (var i = 0; i < callArgsCount; i++)
                    _DefineDataProperty(args, i + 2, arguments[i]);
                return args;
            };
        }

        var args = FUN_APPLY(combiner, null, arguments);
        if (newTarget === undefined)
            return bind_applyFunctionN(fun, thisArg, args);
        return bind_constructFunctionN(fun, newTarget, args);
    };
}

function bind_bindFunctionN(fun, thisArg, boundArgs) {
    assert(boundArgs.length > 2, "Fast paths should be used for few-bound-args cases.");
    var combiner = null;
    return function bound() {
        var newTarget;
        if (_IsConstructing()) {
            newTarget = new.target;
            if (newTarget === bound)
                newTarget = fun;
        }
        if (arguments.length === 0) {
            if (newTarget !== undefined)
                return bind_constructFunctionN(fun, newTarget, boundArgs);
            return bind_applyFunctionN(fun, thisArg, boundArgs);
        }

        if (combiner === null) {
            combiner = function() {
                var boundArgsCount = boundArgs.length;
                var callArgsCount = arguments.length;
                var args = std_Array(boundArgsCount + callArgsCount);
                for (var i = 0; i < boundArgsCount; i++)
                    _DefineDataProperty(args, i, boundArgs[i]);
                for (var i = 0; i < callArgsCount; i++)
                    _DefineDataProperty(args, i + boundArgsCount, arguments[i]);
                return args;
            };
        }

        var args = FUN_APPLY(combiner, null, arguments);
        if (newTarget !== undefined)
            return bind_constructFunctionN(fun, newTarget, args);
        return bind_applyFunctionN(fun, thisArg, args);
    };
}

function bind_mapArguments() {
    var len = arguments.length;
    var args = std_Array(len);
    for (var i = 0; i < len; i++)
        _DefineDataProperty(args, i, arguments[i]);
    return args;
}

function bind_applyFunctionN(fun, thisArg, args) {
    switch (args.length) {
      case 0:
        return callContentFunction(fun, thisArg);
      case 1:
        return callContentFunction(fun, thisArg, SPREAD(args, 1));
      case 2:
        return callContentFunction(fun, thisArg, SPREAD(args, 2));
      case 3:
        return callContentFunction(fun, thisArg, SPREAD(args, 3));
      case 4:
        return callContentFunction(fun, thisArg, SPREAD(args, 4));
      case 5:
        return callContentFunction(fun, thisArg, SPREAD(args, 5));
      case 6:
        return callContentFunction(fun, thisArg, SPREAD(args, 6));
      case 7:
        return callContentFunction(fun, thisArg, SPREAD(args, 7));
      case 8:
        return callContentFunction(fun, thisArg, SPREAD(args, 8));
      case 9:
        return callContentFunction(fun, thisArg, SPREAD(args, 9));
      default:
        return FUN_APPLY(fun, thisArg, args);
    }
}

function bind_constructFunctionN(fun, newTarget, args) {
    switch (args.length) {
      case 1:
        return constructContentFunction(fun, newTarget, SPREAD(args, 1));
      case 2:
        return constructContentFunction(fun, newTarget, SPREAD(args, 2));
      case 3:
        return constructContentFunction(fun, newTarget, SPREAD(args, 3));
      case 4:
        return constructContentFunction(fun, newTarget, SPREAD(args, 4));
      case 5:
        return constructContentFunction(fun, newTarget, SPREAD(args, 5));
      case 6:
        return constructContentFunction(fun, newTarget, SPREAD(args, 6));
      case 7:
        return constructContentFunction(fun, newTarget, SPREAD(args, 7));
      case 8:
        return constructContentFunction(fun, newTarget, SPREAD(args, 8));
      case 9:
        return constructContentFunction(fun, newTarget, SPREAD(args, 9));
      case 10:
        return constructContentFunction(fun, newTarget, SPREAD(args, 10));
      case 11:
        return constructContentFunction(fun, newTarget, SPREAD(args, 11));
      case 12:
        return constructContentFunction(fun, newTarget, SPREAD(args, 12));
      default:
        assert(args.length !== 0,
               "bound function construction without args should be handled by caller");
        return _ConstructFunction(fun, newTarget, args);
    }
}
