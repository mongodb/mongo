//
// This test ensures that the hello command and its aliases, ismaster and isMaster,
// are all accepted.
//
// @tags: [
//    # Assert on the isWritablePrimary field of a hello response. If a primary steps down after
//    # accepting a hello command and returns before its connection is closed, the response can
//    # contain isWritablePrimary: false.
//    does_not_support_stepdowns,
// ]

(function() {
"use strict";

function checkResponseFields(command, commandType) {
    var res = db.runCommand(command);
    // check that the fields that should be there are there and have proper values
    assert(res.maxBsonObjectSize && isNumber(res.maxBsonObjectSize) && res.maxBsonObjectSize > 0,
           "maxBsonObjectSize possibly missing:" + tojson(res));
    assert(
        res.maxMessageSizeBytes && isNumber(res.maxMessageSizeBytes) && res.maxBsonObjectSize > 0,
        "maxMessageSizeBytes possibly missing:" + tojson(res));
    assert(res.maxWriteBatchSize && isNumber(res.maxWriteBatchSize) && res.maxWriteBatchSize > 0,
           "maxWriteBatchSize possibly missing:" + tojson(res));

    if (commandType === "hello") {
        assert.eq("boolean",
                  typeof res.isWritablePrimary,
                  "isWritablePrimary field is not a boolean" + tojson(res));
        assert(res.isWritablePrimary === true, "isWritablePrimary field is false" + tojson(res));
    } else {
        assert.eq("boolean", typeof res.ismaster, "ismaster field is not a boolean" + tojson(res));
        assert(res.ismaster === true, "ismaster field is false" + tojson(res));
    }
    assert(res.localTime, "localTime possibly missing:" + tojson(res));
    assert(res.connectionId, "connectionId missing or false" + tojson(res));

    if (!testingReplication) {
        var badFields = [];
        var unwantedReplSetFields = [
            "setName",
            "setVersion",
            "secondary",
            "hosts",
            "passives",
            "arbiters",
            "primary",
            "arbiterOnly",
            "passive",
            "slaveDelay",
            "secondaryDelaySecs",
            "hidden",
            "tags",
            "buildIndexes",
            "me"
        ];
        var field;
        // check that the fields that shouldn't be there are not there
        for (field in res) {
            if (!res.hasOwnProperty(field)) {
                continue;
            }
            if (Array.contains(unwantedReplSetFields, field)) {
                badFields.push(field);
            }
        }
        assert(
            badFields.length === 0,
            "\nthe result:\n" + tojson(res) + "\ncontained fields it shouldn't have: " + badFields);
    }
}

checkResponseFields("hello", "hello");
checkResponseFields("ismaster", "ismaster");
checkResponseFields("isMaster", "isMaster");
checkResponseFields({hello: 1, unknownField: 1}, "hello");
checkResponseFields({ismaster: 1, unknownField: 1}, "ismaster");
checkResponseFields({isMaster: 1, unknownField: 1}, "isMaster");

// As operations happen concurrently, the response objects may have different timestamps. To compare
// response objects returned from calling commands directly and shell helpers below, we must remove
// the timestamps.
function removeTimestamps(cmdResponse) {
    delete cmdResponse.localTime;
    delete cmdResponse.operationTime;
    delete cmdResponse.$clusterTime;
    if (cmdResponse.lastWrite) {
        delete cmdResponse.lastWrite.opTime;
        delete cmdResponse.lastWrite.majorityOpTime;
    }
}

// We also test the db.hello() and db.isMaster() helpers to ensure that they return
// the same response objects as from running the commands directly.
let cmdResponse1 = removeTimestamps(db.runCommand("hello"));
let cmdResponse2 = removeTimestamps(db.hello());
assert.eq(cmdResponse1, cmdResponse2);

cmdResponse1 = removeTimestamps(db.runCommand("isMaster"));
cmdResponse2 = removeTimestamps(db.isMaster());
assert.eq(cmdResponse1, cmdResponse2);
})();
