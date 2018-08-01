// Test config file expansion using REST at top level.
// @tags: [requires_http_client]

(function() {
    'use strict';

    load('jstests/noPassthrough/libs/configExpand/lib.js');

    const web = new ConfigExpandRestServer();
    web.start();

    // Basic success case
    configExpandSuccess({
        setParameter: {
            scramIterationCount: {__rest: web.getStringReflectionURL('12345')},
            scramSHA256IterationCount:
                {__rest: web.getStringReflectionURL('23456'), type: 'string', trim: 'whitespace'}
        }
    },
                        function(admin) {
                            const response = assert.commandWorked(admin.runCommand({
                                getParameter: 1,
                                scramIterationCount: 1,
                                scramSHA256IterationCount: 1
                            }));
                            assert.eq(response.scramIterationCount,
                                      12345,
                                      "Incorrect derived config value for scramIterationCount");
                            assert.eq(response.scramSHA256IterationCount,
                                      23456,
                                      "Incorrect derived config value scramSHA256IterationCount");
                        });

    web.stop();
})();
