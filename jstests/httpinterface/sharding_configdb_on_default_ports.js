// This test confirms that mongos interprets host names passed to it via the
// --configdb command line argument *without* a port number as referring to
// processes listening on the config server port (27019) rather than the default
// mongod port of 27017.
//
// That is, mongos --configdb=localhost should look for a config server on port 27019,
// not port 27017.
//
// The test confirms this behavior for 1-node config servers, SCCC config servers and
// CSRS config servers.

(function() {
    "use strict";

    function getHostPart(hostAndPort) {
        return hostAndPort.substr(0, hostAndPort.lastIndexOf(':'));
    }
    var c1, c2, c3;

    // The config servers must support readConcern: majority to be run as a replica set, so
    // explicitly set storage engine to wiredTiger.
    c1 = MongoRunner.runMongod({
        configsvr: "",
        port: 27019,
        replSet: "csrs",
        storageEngine: "wiredTiger"
    });
    assert.commandWorked(c1.adminCommand("replSetInitiate"));
    c2 = MongoRunner.runMongod({configsvr: "", storageEngine: "wiredTiger"});
    c3 = MongoRunner.runMongod({configsvr: "", storageEngine: "wiredTiger"});

    var configstrs = [
        getHostPart(c1.host) + "," + c2.host + "," + c3.host,
        getHostPart(c1.host),
        "csrs/" + getHostPart(c1.host)
    ];
    var failureMessages = [];
    configstrs.forEach(function (configdb) {
        if (!MongoRunner.runMongos({configdb: configdb})) {
            failureMessages.push("Failed to start mongos with configdb=\"" + configdb + "\"");
        }
    });
    assert.eq(0, failureMessages.length, tojson(failureMessages));
}());
