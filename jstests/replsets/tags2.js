// Change a write concern mode from 2 to 3 servers
// @tags: [multiversion_incompatible]

import {ReplSetTest} from "jstests/libs/replsettest.js";

let host = getHostName();
let replTest = new ReplSetTest({nodes: 4});
let nodes = replTest.startSet();
let ports = replTest.ports;
let conf = {
    _id: replTest.name,
    members: [
        {_id: 0, host: host + ":" + ports[0], tags: {"backup": "A"}},
        {_id: 1, host: host + ":" + ports[1], tags: {"backup": "B"}},
        {_id: 2, host: host + ":" + ports[2], tags: {"backup": "C"}},
        {_id: 3, host: host + ":" + ports[3], tags: {"backup": "D"}, arbiterOnly: true},
    ],
    settings: {getLastErrorModes: {backedUp: {backup: 2}}},
};

print("arbiters can't have tags");
let result = nodes[0].getDB("admin").runCommand({replSetInitiate: conf});
printjson(result);
assert.eq(result.ok, 0);

conf.members.pop();
replTest.stop(3);
replTest.remove(3);
replTest.initiate(conf);

replTest.awaitReplication();

let primary = replTest.getPrimary();
var db = primary.getDB("test");
let wtimeout = ReplSetTest.kDefaultTimeoutMS;

assert.commandWorked(db.foo.insert({x: 1}, {writeConcern: {w: "backedUp", wtimeout: wtimeout}}));

let nextVersion = replTest.getReplSetConfigFromNode().version + 1;
conf.version = nextVersion;
conf.settings.getLastErrorModes.backedUp.backup = 3;
primary.getDB("admin").runCommand({replSetReconfig: conf});
replTest.awaitReplication();

primary = replTest.getPrimary();
var db = primary.getDB("test");
assert.commandWorked(db.foo.insert({x: 2}, {writeConcern: {w: "backedUp", wtimeout: wtimeout}}));

nextVersion++;
conf.version = nextVersion;
conf.members[0].priorty = 3;
conf.members[2].priorty = 0;
primary.getDB("admin").runCommand({replSetReconfig: conf});

primary = replTest.getPrimary();
var db = primary.getDB("test");
assert.commandWorked(db.foo.insert({x: 3}, {writeConcern: {w: "backedUp", wtimeout: wtimeout}}));

replTest.stopSet();
