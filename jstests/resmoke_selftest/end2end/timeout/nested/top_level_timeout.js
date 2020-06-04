(function() {
'use strict';

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

function start() {
    const resmokeCmd = 'python3 buildscripts/resmoke.py run ' +
        '--storageEngineCacheSizeGB=1 --dbpathPrefix=/data/db/selftest_inner ' +
        '--internalParam=test_archival --taskId=123 ' +
        '--internalParam=is_inner_level ' +
        '--basePort=20020 ' +
        '--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_nested_timeout.yml ' +
        'jstests/resmoke_selftest/end2end/timeout/nested/inner_level_timeout.js';

    // Start a new resmoke test
    return _startMongoProgram({args: resmokeCmd.split(' ')});
}

const pid = start();

while (true) {
    checkProgram(pid);
    print("looping");
    sleep(1000);
}
})();
