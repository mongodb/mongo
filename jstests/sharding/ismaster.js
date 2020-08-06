/**
 * This test ensures that the hello command and its aliases, ismaster and isMaster, are all
 * accepted by mongos.
 */
"use strict";
var st = new ShardingTest({shards: 1, mongos: 1});

function checkResponseFields(commandString) {
    jsTestLog("Running the " + commandString + " command");
    var res = st.s0.getDB("admin").runCommand(commandString);

    // check that the fields that should be there are there and have proper values
    assert(res.maxBsonObjectSize && isNumber(res.maxBsonObjectSize) && res.maxBsonObjectSize > 0,
           "maxBsonObjectSize possibly missing:" + tojson(res));
    assert(
        res.maxMessageSizeBytes && isNumber(res.maxMessageSizeBytes) && res.maxBsonObjectSize > 0,
        "maxMessageSizeBytes possibly missing:" + tojson(res));

    if (commandString === "hello") {
        assert.eq("boolean",
                  typeof res.isWritablePrimary,
                  "isWritablePrimary field is not a boolean" + tojson(res));
        assert(res.isWritablePrimary === true, "isWritablePrimary field is false" + tojson(res));
    } else {
        assert.eq("boolean", typeof res.ismaster, "ismaster field is not a boolean" + tojson(res));
        assert(res.ismaster === true, "ismaster field is false" + tojson(res));
    }

    assert(res.localTime, "localTime possibly missing:" + tojson(res));
    assert(res.msg && res.msg == "isdbgrid", "msg possibly missing or wrong:" + tojson(res));

    var unwantedFields = [
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
    // check that the fields that shouldn't be there are not there
    var badFields = [];
    var field;
    for (field in res) {
        if (!res.hasOwnProperty(field)) {
            continue;
        }
        if (Array.contains(unwantedFields, field)) {
            badFields.push(field);
        }
    }
    assert(badFields.length === 0,
           "\nthe result:\n" + tojson(res) + "\ncontained fields it shouldn't have: " + badFields);
}

checkResponseFields("hello");
checkResponseFields("ismaster");
checkResponseFields("isMaster");

st.stop();
