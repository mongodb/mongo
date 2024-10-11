/**
 * Test starting up a mongos process and immediately shutting it down.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    forkThenShutdownMongos,
    startThenShutdownMongos
} from "jstests/noPassthrough/libs/startup_shutdown_helpers.js";

// For mongos to start up successfully, it needs a config server to connect to.
const configRS = new ReplSetTest({nodes: 1});
configRS.startSet({configsvr: ''});
configRS.initiate();

startThenShutdownMongos(configRS, {});

// --fork is not available on Windows.
if (!_isWindows()) {
    forkThenShutdownMongos(configRS, {});
}

configRS.stopSet();
