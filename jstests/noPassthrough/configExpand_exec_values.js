// Test config file expansion using EXEC.

(function() {
'use strict';

load('jstests/noPassthrough/libs/configExpand/lib.js');

// Basic success case
configExpandSuccess(
    {
        setParameter: {
            scramIterationCount: {__exec: makeReflectionCmd('12345')},
            scramSHA256IterationCount:
                {__exec: makeReflectionCmd("23456\n"), type: 'string', trim: 'whitespace'}
        }
    },
    function(admin) {
        const response = assert.commandWorked(admin.runCommand(
            {getParameter: 1, scramIterationCount: 1, scramSHA256IterationCount: 1}));
        assert.eq(response.scramIterationCount,
                  12345,
                  "Incorrect derived config value for scramIterationCount");
        assert.eq(response.scramSHA256IterationCount,
                  23456,
                  "Incorrect derived config value scramSHA256IterationCount");
    });
})();
