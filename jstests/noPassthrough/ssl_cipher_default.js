// validate default for opensslCipherConfig

(function() {
'use strict';

function getparam(mongod, field) {
    var q = {getParameter: 1};
    q[field] = 1;

    var ret = mongod.getDB("admin").runCommand(q);
    return ret[field];
}

function assertCorrectConfig(mongodArgs, expectedConfig) {
    let m = MongoRunner.runMongod(mongodArgs);
    assert.eq(getparam(m, "opensslCipherConfig"), expectedConfig);
    MongoRunner.stopMongod(m);
}

const defaultConfig = "HIGH:!EXPORT:!aNULL@STRENGTH";

// if sslMode is disabled, cipher config should be set to default
assertCorrectConfig({sslMode: 'disabled'}, defaultConfig);

// if sslMode is enabled, cipher config should have default
assertCorrectConfig({
    sslMode: 'allowSSL',
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem"
},
                    defaultConfig);

// setting through setParameter or tlsCipherConfig should override default
assertCorrectConfig({
    sslMode: 'allowSSL',
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem",
    setParameter: "opensslCipherConfig=HIGH"
},
                    "HIGH");

assertCorrectConfig({
    sslMode: 'allowSSL',
    sslPEMKeyFile: "jstests/libs/server.pem",
    sslCAFile: "jstests/libs/ca.pem",
    tlsCipherConfig: "HIGH"
},
                    "HIGH");
})();
