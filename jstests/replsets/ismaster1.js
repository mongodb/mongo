/**
 * check all the easily testable fields in the document returned by the isMaster() command
 * also checks that fields that should not be in the document are absent
 */

load("jstests/replsets/rslib.js");

// function create the error message if an assert fails
var generateErrorString = function(badFields, missingFields, badValues, result) {
    var str = "\nThe result was:\n" + tojson(result);
    if (badFields.length !== 0) {
        str += "\nIt had the following fields which it shouldn't have: ";
        str += badFields;
    }
    if (missingFields.length !== 0) {
        str += "\nIt lacked the following fields which it should have contained: ";
        str += missingFields;
    }
    if (badValues.length !== 0) {
        for (i = 0; i < badValues.length; i += 3) {
            str += "\nIts value for " + badValues[i] + " is " + badValues[i + 1];
            str += " but should be " + badValues[i + 2];
        }
    }
    return str;
};

// function to check a single result
var checkMember = function(memberInfo) {
    // run isMaster on the connection
    result = memberInfo.conn.getDB("admin").runCommand({isMaster: 1});

    // make sure result doesn't contain anything it shouldn't
    var badFields = [];
    for (field in result) {
        if (!result.hasOwnProperty(field)) {
            continue;
        }
        if (Array.contains(memberInfo.unwantedFields, field)) {
            badFields.push(field);
        }
    }

    // make sure result contains the fields we want
    var missingFields = [];
    for (i = 0; i < memberInfo.wantedFields.length; i++) {
        field = memberInfo.wantedFields[i];
        if (!result.hasOwnProperty(field)) {
            missingFields.push(field);
            print(field);
        }
    }

    // make sure the result has proper values for fields with known values
    var badValues = [];  // each mistake will be saved as three entries (key, badvalue, goodvalue)
    for (field in memberInfo.goodValues) {
        if (typeof(memberInfo.goodValues[field]) === "object") {
            // assumes nested obj is disk in tags this is currently true, but may change
            if (result[field].disk !== memberInfo.goodValues[field].disk) {
                badValues.push("tags.disk");
                badValues.push(result[field].disk);
                badValues.push(memberInfo.goodValues[field].disk);
            }
        } else {
            if (result[field] !== memberInfo.goodValues[field]) {
                badValues.push(field);
                badValues.push(result[field]);
                badValues.push(memberInfo.goodValues[field]);
            }
        }
    }
    assert(badFields.length === 0 && missingFields.length === 0 && badValues.length === 0,
           memberInfo.name + " had the following problems." +
               generateErrorString(badFields, missingFields, badValues, result));
};

// start of test code
var name = "ismaster";
var replTest = new ReplSetTest({name: name, nodes: 4});
var nodes = replTest.startSet();

var config = replTest.getReplSetConfig();
config.members[1].priority = 0;
config.members[2].priority = 0;
config.members[2].hidden = true;
config.members[2].slaveDelay = 3;
config.members[2].buildIndexes = false;
config.members[3].arbiterOnly = true;
replTest.initiate(config);

var agreeOnPrimaryAndSetVersion = function(setVersion) {

    print("Waiting for primary and replica set version " + setVersion);

    var nodes = replTest.nodes;
    var primary = undefined;
    var lastSetVersion = setVersion;
    for (var i = 0; i < nodes.length; i++) {
        try {
            var isMasterResult = nodes[i].getDB("admin").runCommand({isMaster: 1});
        } catch (e) {
            // handle reconnect errors due to step downs
            print("Error while calling isMaster on " + nodes[i] + ": " + e);
            return false;
        }

        printjson(isMasterResult);
        if (!primary)
            primary = isMasterResult.primary;
        if (!lastSetVersion)
            lastSetVersion = isMasterResult.setVersion;
        if (isMasterResult.primary != primary || !primary)
            return false;
        if (isMasterResult.setVersion != lastSetVersion)
            return false;
    }

    return true;
};

var master = replTest.getPrimary();
assert.soon(function() {
    return agreeOnPrimaryAndSetVersion(1);
}, "Nodes did not initiate in less than a minute", 60000);

// check to see if the information from isMaster() is correct at each node
// the checker only checks that the field exists when its value is "has"
checkMember({
    conn: master,
    name: "master",
    goodValues: {setName: "ismaster", setVersion: 1, ismaster: true, secondary: false, ok: 1},
    wantedFields:
        ["hosts", "passives", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: ["arbiterOnly", "passive", "slaveDelay", "hidden", "tags", "buildIndexes"]
});

checkMember({
    conn: replTest.liveNodes.slaves[0],
    name: "slave",
    goodValues: {
        setName: "ismaster",
        setVersion: 1,
        ismaster: false,
        secondary: true,
        passive: true,
        ok: 1
    },
    wantedFields:
        ["hosts", "passives", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: ["arbiterOnly", "slaveDelay", "hidden", "tags", "buildIndexes"]
});

checkMember({
    conn: replTest.liveNodes.slaves[1],
    name: "delayed_slave",
    goodValues: {
        setName: "ismaster",
        setVersion: 1,
        ismaster: false,
        secondary: true,
        passive: true,
        slaveDelay: 3,
        buildIndexes: false,
        ok: 1
    },
    wantedFields:
        ["hosts", "passives", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: ["arbiterOnly", "tags"]
});

checkMember({
    conn: replTest.liveNodes.slaves[2],
    name: "arbiter",
    goodValues: {
        setName: "ismaster",
        setVersion: 1,
        ismaster: false,
        secondary: false,
        arbiterOnly: true,
        ok: 1
    },
    wantedFields:
        ["hosts", "passives", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: ["slaveDelay", "hidden", "tags", "buildIndexes", "passive"]
});

// reconfigure and make sure the changes show up in ismaster on all members
config = master.getDB("local").system.replset.findOne();
config.version = config.version + 1;
config.members[0].tags = {
    disk: "ssd"
};
config.members[1].tags = {
    disk: "ssd"
};
config.members[1].hidden = true;
config.members[2].slaveDelay = 300000;
config.members[2].tags = {
    disk: "hdd"
};
try {
    result = master.getDB("admin").runCommand({replSetReconfig: config});
} catch (e) {
    print(e);
}

master = replTest.getPrimary();
assert.soon(function() {
    return agreeOnPrimaryAndSetVersion(2);
}, "Nodes did not sync in less than a minute", 60000);

// check nodes for their new settings
checkMember({
    conn: master,
    name: "master2",
    goodValues: {
        setName: "ismaster",
        setVersion: 2,
        ismaster: true,
        secondary: false,
        tags: {"disk": "ssd"},
        ok: 1
    },
    wantedFields: ["hosts", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields:
        ["arbiterOnly", "passives", "passive", "slaveDelay", "hidden", "buildIndexes"]
});

checkMember({
    conn: replTest.liveNodes.slaves[0],
    name: "first_slave",
    goodValues: {
        setName: "ismaster",
        setVersion: 2,
        ismaster: false,
        secondary: true,
        tags: {"disk": "ssd"},
        passive: true,
        hidden: true,
        ok: 1
    },
    wantedFields: ["hosts", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: ["arbiterOnly", "passives", "slaveDelayed", "buildIndexes"]
});

checkMember({
    conn: replTest.liveNodes.slaves[1],
    name: "very_delayed_slave",
    goodValues: {
        setName: "ismaster",
        setVersion: 2,
        ismaster: false,
        secondary: true,
        tags: {"disk": "hdd"},
        passive: true,
        slaveDelay: 300000,
        buildIndexes: false,
        hidden: true,
        ok: 1
    },
    wantedFields: ["hosts", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: ["arbiterOnly", "passives"]
});

checkMember({
    conn: replTest.liveNodes.slaves[2],
    name: "arbiter",
    goodValues: {
        setName: "ismaster",
        setVersion: 2,
        ismaster: false,
        secondary: false,
        arbiterOnly: true,
        ok: 1
    },
    wantedFields: ["hosts", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: ["slaveDelay", "hidden", "tags", "buildIndexes", "passive"]
});

// force reconfig and ensure all have the same setVersion afterwards
config = master.getDB("local").system.replset.findOne();
master.getDB("admin").runCommand({replSetReconfig: config, force: true});

assert.soon(function() {
    return agreeOnPrimaryAndSetVersion();
}, "Nodes did not sync in less than a minute after forced reconfig", 60000);

replTest.stopSet();
