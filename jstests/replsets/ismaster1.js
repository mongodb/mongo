/**
 * Checks all the easily testable fields in the response object returned by the hello() command and
 * its aliases, isMaster() and ismaster(). This test also checks that fields that should not be in
 * the document are absent.
 */

// Skip db hash check because node 2 is slave delayed and may time out on awaitReplication.
TestData.skipCheckDBHashes = true;

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

// This function calls checkResponseFields with the isMaster and hello commands.
var runHelloCmdAndAliases = function(memberInfo) {
    checkResponseFields(memberInfo, "ismaster");
    checkResponseFields(memberInfo, "isMaster");
    checkResponseFields(memberInfo, "hello");
};

// This function runs either the isMaster or hello command, and validates that the response is what
// we expect.
var checkResponseFields = function(memberInfo, cmd) {
    // run the passed in command on the connection
    var result = memberInfo.conn.getDB("admin").runCommand(cmd);
    // If we are running the hello command, we must modify the expected fields. We expect
    // "isWritablePrimary" and "secondaryDelaySecs" instead of "ismaster" and "slaveDelay" in the
    // hello command response.
    if (cmd === "hello") {
        memberInfo.goodValues.isWritablePrimary = memberInfo.goodValues.ismaster;
        delete memberInfo.goodValues.ismaster;
        memberInfo.unwantedFields.push("ismaster");
        memberInfo.unwantedFields =
            memberInfo.unwantedFields.filter(f => f !== "isWritablePrimary");

        if (memberInfo.goodValues.hasOwnProperty("slaveDelay")) {
            memberInfo.goodValues.secondaryDelaySecs = memberInfo.goodValues.slaveDelay;
            delete memberInfo.goodValues.slaveDelay;
            memberInfo.unwantedFields.push("slaveDelay");
            memberInfo.unwantedFields =
                memberInfo.unwantedFields.filter(f => f !== "secondaryDelaySecs");
        }
    }

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
var name = "hello_and_aliases";
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
    var currPrimary = undefined;
    var lastSetVersion = setVersion;
    for (var i = 0; i < nodes.length; i++) {
        try {
            var helloResult = nodes[i].getDB("admin").runCommand({hello: 1});
        } catch (e) {
            // handle reconnect errors due to step downs
            print("Error while calling hello on " + nodes[i] + ": " + e);
            return false;
        }

        printjson(helloResult);
        if (!currPrimary)
            currPrimary = helloResult.primary;
        if (!lastSetVersion)
            lastSetVersion = helloResult.setVersion;
        if (helloResult.primary != currPrimary || !currPrimary)
            return false;
        if (helloResult.setVersion != lastSetVersion)
            return false;
    }

    return true;
};

var primary = replTest.getPrimary();
var expectedVersion = replTest.getReplSetConfigFromNode().version;
assert.soon(function() {
    return agreeOnPrimaryAndSetVersion(expectedVersion);
}, "Nodes did not initiate in less than a minute", 60000);

// Check to see if the information from hello() and its aliases are correct at each node.
// The checker only checks that the field exists when its value is "has".
runHelloCmdAndAliases({
    conn: primary,
    name: "primary",
    goodValues: {
        setName: "hello_and_aliases",
        setVersion: expectedVersion,
        ismaster: true,
        secondary: false,
        ok: 1
    },
    wantedFields:
        ["hosts", "passives", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: [
        "isWritablePrimary",
        "arbiterOnly",
        "passive",
        "slaveDelay",
        "secondaryDelaySecs",
        "hidden",
        "tags",
        "buildIndexes"
    ]
});

runHelloCmdAndAliases({
    conn: replTest.liveNodes.slaves[0],
    name: "secondary",
    goodValues: {
        setName: "hello_and_aliases",
        setVersion: expectedVersion,
        ismaster: false,
        secondary: true,
        passive: true,
        ok: 1
    },
    wantedFields:
        ["hosts", "passives", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: [
        "isWritablePrimary",
        "arbiterOnly",
        "slaveDelay",
        "secondaryDelaySecs",
        "hidden",
        "tags",
        "buildIndexes"
    ]
});

runHelloCmdAndAliases({
    conn: replTest.liveNodes.slaves[1],
    name: "delayed_secondary",
    goodValues: {
        setName: "hello_and_aliases",
        setVersion: expectedVersion,
        ismaster: false,
        secondary: true,
        passive: true,
        slaveDelay: 3,
        buildIndexes: false,
        ok: 1
    },
    wantedFields:
        ["hosts", "passives", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: ["isWritablePrimary", "arbiterOnly", "tags", "secondaryDelaySecs"]
});

runHelloCmdAndAliases({
    conn: replTest.liveNodes.slaves[2],
    name: "arbiter",
    goodValues: {
        setName: "hello_and_aliases",
        setVersion: expectedVersion,
        ismaster: false,
        secondary: false,
        arbiterOnly: true,
        ok: 1
    },
    wantedFields:
        ["hosts", "passives", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: [
        "isWritablePrimary",
        "slaveDelay",
        "secondaryDelaySecs",
        "hidden",
        "tags",
        "buildIndexes",
        "passive"
    ]
});

// Reconfigure the replset and make sure the changes are present on all members.
config = primary.getDB("local").system.replset.findOne();
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
    result = primary.getDB("admin").runCommand({replSetReconfig: config});
} catch (e) {
    print(e);
}

primary = replTest.getPrimary();
expectedVersion = config.version;
assert.soon(function() {
    return agreeOnPrimaryAndSetVersion(expectedVersion);
}, "Nodes did not sync in less than a minute", 60000);

// check nodes for their new settings
runHelloCmdAndAliases({
    conn: primary,
    name: "primary2",
    goodValues: {
        setName: "hello_and_aliases",
        setVersion: expectedVersion,
        ismaster: true,
        secondary: false,
        tags: {"disk": "ssd"},
        ok: 1
    },
    wantedFields: ["hosts", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: [
        "isWritablePrimary",
        "arbiterOnly",
        "passives",
        "passive",
        "slaveDelay",
        "secondaryDelaySecs",
        "hidden",
        "buildIndexes"
    ]
});

runHelloCmdAndAliases({
    conn: replTest.liveNodes.slaves[0],
    name: "first_secondary",
    goodValues: {
        setName: "hello_and_aliases",
        setVersion: expectedVersion,
        ismaster: false,
        secondary: true,
        tags: {"disk": "ssd"},
        passive: true,
        hidden: true,
        ok: 1
    },
    wantedFields: ["hosts", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: [
        "isWritablePrimary",
        "arbiterOnly",
        "passives",
        "slaveDelay",
        "secondaryDelaySecs",
        "buildIndexes"
    ]
});

runHelloCmdAndAliases({
    conn: replTest.liveNodes.slaves[1],
    name: "very_delayed_secondary",
    goodValues: {
        setName: "hello_and_aliases",
        setVersion: expectedVersion,
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
    unwantedFields: ["isWritablePrimary", "arbiterOnly", "passives", "secondaryDelaySecs"]
});

runHelloCmdAndAliases({
    conn: replTest.liveNodes.slaves[2],
    name: "arbiter",
    goodValues: {
        setName: "hello_and_aliases",
        setVersion: expectedVersion,
        ismaster: false,
        secondary: false,
        arbiterOnly: true,
        ok: 1
    },
    wantedFields: ["hosts", "arbiters", "primary", "me", "maxBsonObjectSize", "localTime"],
    unwantedFields: [
        "isWritablePrimary",
        "slaveDelay",
        "secondaryDelaySecs",
        "hidden",
        "tags",
        "buildIndexes",
        "passive"
    ]
});

// force reconfig and ensure all have the same setVersion afterwards
config = primary.getDB("local").system.replset.findOne();
primary.getDB("admin").runCommand({replSetReconfig: config, force: true});

assert.soon(function() {
    return agreeOnPrimaryAndSetVersion();
}, "Nodes did not sync in less than a minute after forced reconfig", 60000);

replTest.stopSet();
