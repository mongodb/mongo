// Make sure the psuedo-option --tlsOnNormalPorts is correctly canonicalized.

(function() {
'use strict';

const mongod = MongoRunner.runMongod({
    tlsOnNormalPorts: '',
    tlsCertificateKeyFile: 'jstests/libs/server.pem',
});
assert(mongod);
assert.commandWorked(mongod.getDB('admin').runCommand({hello: 1}));
MongoRunner.stopMongod(mongod);
})();
