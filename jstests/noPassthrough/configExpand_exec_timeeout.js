// Test config file expansion using EXEC.

(function() {
'use strict';

load('jstests/noPassthrough/libs/configExpand/lib.js');

assert.eq(runNonMongoProgram.apply(null, makeReflectionCmd('12345', {sleep: 0}).split(" ")), 0);

// Sleep 10 seconds during request.
configExpandSuccess({
    setParameter: {
        scramIterationCount: {__exec: makeReflectionCmd('12345', {sleep: 10})},
    }
});

// Sleep 40 seconds during request, with default 30 second timeout.
configExpandFailure({
    setParameter: {
        scramIterationCount: {__exec: makeReflectionCmd('12345', {sleep: 40})},
    }
},
                    /Timeout expired/);

// Sleep 10 seconds during request, with custom 5 second timeout.
configExpandFailure({
    setParameter: {
        scramIterationCount: {__exec: makeReflectionCmd('12345', {sleep: 10})},
    }
},
                    /Timeout expired/,
                    {configExpandTimeoutSecs: 5});
})();
