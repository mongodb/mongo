// Exports 'extendedAssert' which includes all built in assertions and:
//  - New extendedAssert.strContains(needle, haystack, msg)
//  - a .soon variant of eq, neq, contains, gt, lt, gte, lte, and strContains
//      e.g. .eq.soon(expected, getActualFunc, msg[, timeout, interval])
//      This produces more descriptive assertion error messages than the built
//      in assert.soon provides.

var extendedAssert;
(function() {
  if (typeof extendedAssert !== 'undefined') {
    return;
  }

  // Make a copy of the assert object
  extendedAssert = assert.bind(this);
  for (var key in assert) {
    if (assert.hasOwnProperty(key)) {
      extendedAssert[key] = assert[key];
    }
  }

  extendedAssert.strContains = function(needle, haystack, msg) {
    if (haystack.indexOf(needle) === -1) {
      doassert('"' + haystack + '" does not contain "' + needle + '" : ' + msg);
    }
  };

  var EX_ASSERT_DONT_PRINT = '**extended_assert.js--do not print this error message**';
  var builtin_doassert = doassert;
  var muteable_doassert = function(msg, obj) {
    if (msg.indexOf(EX_ASSERT_DONT_PRINT) !== -1) {
      throw Error(msg);
    }
    builtin_doassert(msg, obj);
  };

  ['eq', 'neq', 'contains', 'gt', 'lt', 'gte', 'lte', 'strContains']
    .forEach(function (name) {
      var assertFunc = extendedAssert[name];
      var newAssertFunc = assertFunc.bind(this);
      newAssertFunc.soon = function(expected, actualFunc, msg, timeout, interval) {
        try {
          doassert = muteable_doassert;
          extendedAssert.soon(function() {
            try {
              assertFunc(expected, actualFunc(), EX_ASSERT_DONT_PRINT);
              return true;
            } catch (e) {
              return false;
            }
          }, EX_ASSERT_DONT_PRINT, timeout, interval);
          doassert = builtin_doassert;
        } catch (e) {
          doassert = builtin_doassert;
        // Make it fail
          assertFunc(expected, actualFunc(), msg);
        }
      };
      extendedAssert[name] = newAssertFunc;
    });
}());
