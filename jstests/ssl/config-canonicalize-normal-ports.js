// Make sure the psuedo-option --tlsOnNormalPorts is correctly canonicalized.

const mongod = MongoRunner.runMongod({
    tlsOnNormalPorts: "",
    tlsCertificateKeyFile: getX509Path("server.pem"),
    tlsCAFile: getX509Path("ca.pem"),
});
assert(mongod);
assert.commandWorked(mongod.getDB("admin").runCommand({hello: 1}));
MongoRunner.stopMongod(mongod);
