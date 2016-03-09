var res = db.isMaster();
// check that the fields that should be there are there and have proper values
assert(res.maxBsonObjectSize && isNumber(res.maxBsonObjectSize) && res.maxBsonObjectSize > 0,
       "maxBsonObjectSize possibly missing:" + tojson(res));
assert(res.maxMessageSizeBytes && isNumber(res.maxMessageSizeBytes) && res.maxBsonObjectSize > 0,
       "maxMessageSizeBytes possibly missing:" + tojson(res));
assert(res.maxWriteBatchSize && isNumber(res.maxWriteBatchSize) && res.maxWriteBatchSize > 0,
       "maxWriteBatchSize possibly missing:" + tojson(res));
assert(res.ismaster, "ismaster missing or false:" + tojson(res));
assert(res.localTime, "localTime possibly missing:" + tojson(res));

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
    // check that the fields that shouldn't be there are not there
    for (field in res) {
        if (!res.hasOwnProperty(field)) {
            continue;
        }
        if (Array.contains(unwantedReplSetFields, field)) {
            badFields.push(field);
        }
    }
    assert(badFields.length === 0,
           "\nthe result:\n" + tojson(res) + "\ncontained fields it shouldn't have: " + badFields);
}
