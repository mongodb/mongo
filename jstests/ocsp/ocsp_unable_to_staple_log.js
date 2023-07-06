// Check that log messages for OCSP stapling work
// @tags: [requires_http_client, requires_ocsp_stapling]

import {
    OCSP_CA_PEM,
    OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM,
    supportsStapling,
    waitForServer
} from "jstests/ocsp/lib/ocsp_helpers.js";

if (!supportsStapling()) {
    quit();
}

const logPath = MongoRunner.dataPath + "mongod.log";

const ocsp_options = {
    logpath: logPath,
    sslMode: "requireSSL",
    sslPEMKeyFile: OCSP_SERVER_SIGNED_BY_INTERMEDIATE_CA_PEM,
    sslCAFile: OCSP_CA_PEM,
    sslAllowInvalidHostnames: "",
    waitForConnect: false,
};

// Because waitForConnect is off, we need to wait for the process to create the
// mongod logfile, hence the waitForServer.
const conn = MongoRunner.runMongod(ocsp_options);
waitForServer(conn);

const failedToStapleID = 5512202;
assert.soon(() => {
    try {
        cat(logPath);
        return true;
    } catch (e) {
        return false;
    }
});
assert.soon(() => {
    return cat(logPath).trim().split("\n").some((line) => JSON.parse(line).id === failedToStapleID);
});

MongoRunner.stopMongod(conn);
