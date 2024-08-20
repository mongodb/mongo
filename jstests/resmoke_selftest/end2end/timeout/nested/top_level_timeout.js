import {getPython3Binary} from "jstests/libs/python.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

function start() {
    // The --originSuite argument is to trick the resmoke local invocation into passing
    // because when we pass --taskId into resmoke it thinks that it is being ran in evergreen
    // and cannot normally find an evergreen task associated with
    // buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_nested_timeout.yml
    const resmokeCmd = getPython3Binary() + ' buildscripts/resmoke.py run ' +
        '--storageEngineCacheSizeGB=1 --dbpathPrefix=/data/db/selftest_inner ' +
        '--internalParam=test_archival --taskId=123 ' +
        '--originSuite=resmoke_end2end_tests ' +
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
