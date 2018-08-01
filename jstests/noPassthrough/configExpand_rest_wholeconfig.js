// Test config file expansion using REST at top level.
// @tags: [requires_http_client]

(function() {
    'use strict';

    load('jstests/noPassthrough/libs/configExpand/lib.js');

    const web = new ConfigExpandRestServer();
    web.start();

    const jsonConfig = JSON.stringify({setParameter: {scramIterationCount: 12345}});
    configExpandSuccess(
        {__rest: web.getURL() + '/reflect/yaml?json=' + encodeURI(jsonConfig), type: 'yaml'},
        function(admin) {
            const response =
                assert.commandWorked(admin.runCommand({getParameter: 1, scramIterationCount: 1}));
            assert.eq(response.scramIterationCount, 12345, "Incorrect derived config value");
        });

    web.stop();
})();
