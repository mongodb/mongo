/**
 * Test starting up a mongod process with various options and immediately shutting it down.
 */

import {
    forkThenShutdownMongod,
    startThenShutdownMongod
} from "jstests/noPassthrough/libs/startup_shutdown_helpers.js";

const argSets = [
    {},
    {shardsvr: "", replSet: "rs0"},
    {configsvr: "", replSet: "rs0"},
    {maintenanceMode: "standalone"},
    {maintenanceMode: "replicaSet"},
];

argSets.forEach(args => {
    startThenShutdownMongod(args);

    // --fork is not available on Windows.
    if (!_isWindows()) {
        forkThenShutdownMongod(args);
    }
});
