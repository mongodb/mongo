// Make sure the psuedo-option --tlsOnNormalPorts is correctly canonicalized.

const mongod = MongoRunner.runMongod({
    tlsOnNormalPorts: '',
    tlsCertificateKeyFile: 'jstests/libs/server.pem',
    tlsCAFile: 'jstests/libs/ca.pem'
});
assert(mongod);
assert.commandWorked(mongod.getDB('admin').runCommand({hello: 1}));
MongoRunner.stopMongod(mongod);