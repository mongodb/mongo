// Test config file expansion using REST at top level.
// @tags: [requires_http_client]

(function() {
'use strict';

load('jstests/noPassthrough/libs/configExpand/lib.js');

const web = new ConfigExpandRestServer();
web.start();

// Sleep 10 seconds during request.
configExpandSuccess({
    setParameter: {
        scramIterationCount: {__rest: web.getStringReflectionURL('12345', {sleep: 10})},
    }
});

// Sleep 40 seconds during request, with default 30 second timeout.
configExpandFailure({
    setParameter: {
        scramIterationCount: {__rest: web.getStringReflectionURL('12345', {sleep: 40})},
    }
},
                    /Timeout was reached/);

// Sleep 10 seconds during request, with custom 5 second timeout.
configExpandFailure({
    setParameter: {
        scramIterationCount: {__rest: web.getStringReflectionURL('12345', {sleep: 10})},
    }
},
                    /Timeout was reached/,
                    {configExpandTimeoutSecs: 5});

web.stop();
})();
