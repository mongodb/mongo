import {ReplSetTest} from "jstests/libs/replsettest.js";

var rst = new ReplSetTest({
    name: 'rshbcrash',
    nodes: {n0: {}},
    nodeOptions: {setParameter: {logComponentVerbosity: tojson({replication: 5})}}
});
rst.startSet();
rst.initiate();
var primary = rst.getPrimary();
var response1 = primary.getDB('admin').adminCommand({
    replSetHeartbeat: 'rshbcrash',
    version: 1,
    term: 0,
    configTerm: 0,
    configVersion: 0,
    from: primary.name
});
assert(response1.ok, "Initial heartbeat failed. Heartbeat response: " + response1);

// By sending a heartbeat request to a server, setting the "from" field
// of the request to the host+port of that server and reporting
// knowledge of a configVersion ahead of the actual config version the
// server knows about, the server should not crash.
jsTestLog("sending heartbeat request with a higher configVersion. Config version sent: " +
          (response1.config.version + 1));
var response2 = primary.getDB('admin').adminCommand({
    replSetHeartbeat: 'rshbcrash',
    version: 1,
    term: response1.term,
    configTerm: response1.configTerm,
    configVersion: response1.config.version + 1,
    from: primary.name
});
assert(response2.ok,
       "Heartbeat with a higher configVersion failed. Heartbeat response: " + response2);
// Make sure that the primary exited early due to BadValue.
checkLog.contains(primary, 10456300);

rst.stopSet();
