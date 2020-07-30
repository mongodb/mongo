/**
 * This test ensures that the hello command and its aliases, ismaster and isMaster, are all
 * accepted.
 * @tags: [requires_fcv_47]
 */
"use strict";

function checkResponseFields(commandString) {
    var res = db.runCommand(commandString);
    // check that the fields that should be there are there and have proper values
    assert(res.maxBsonObjectSize && isNumber(res.maxBsonObjectSize) && res.maxBsonObjectSize > 0,
           "maxBsonObjectSize possibly missing:" + tojson(res));
    assert(
        res.maxMessageSizeBytes && isNumber(res.maxMessageSizeBytes) && res.maxBsonObjectSize > 0,
        "maxMessageSizeBytes possibly missing:" + tojson(res));
    assert(res.maxWriteBatchSize && isNumber(res.maxWriteBatchSize) && res.maxWriteBatchSize > 0,
           "maxWriteBatchSize possibly missing:" + tojson(res));
    assert.eq("boolean", typeof res.ismaster, "ismaster field is not a boolean" + tojson(res));

    // TODO SERVER-49988: Check for res.isWritablePrimary instead of res.ismaster if a hello command
    // was executed.
    assert(res.ismaster === true, "ismaster field is false" + tojson(res));
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
            "aribterOnly",
            "passive",
            "slaveDelay",
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

checkResponseFields("hello");
checkResponseFields("ismaster");
checkResponseFields("isMaster");
