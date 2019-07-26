// Test config file expansion using EXEC at top level.

(function() {
'use strict';

load('jstests/noPassthrough/libs/configExpand/lib.js');

const yamlConfig = jsToYaml({setParameter: {scramIterationCount: 12345}});
configExpandSuccess({__exec: makeReflectionCmd(yamlConfig), type: 'yaml'}, function(admin) {
    const response =
        assert.commandWorked(admin.runCommand({getParameter: 1, scramIterationCount: 1}));
    assert.eq(response.scramIterationCount, 12345, "Incorrect derived config value");
});
})();
