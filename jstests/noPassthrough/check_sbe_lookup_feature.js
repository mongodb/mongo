(function() {
'use strict';

const conn = MongoRunner.runMongod();

const res = assert.commandWorked(
    conn.getDB("admin").adminCommand({getParameter: 1, featureFlagSBELookupPushdown: 1}),
    "featureFlagSBELookupPushdown must have been turned on by default since 6.0");
assert(res.hasOwnProperty("featureFlagSBELookupPushdown"), res);
const featureFlag = res.featureFlagSBELookupPushdown;
assert(featureFlag.hasOwnProperty("value") && featureFlag.value, res);
assert(featureFlag.hasOwnProperty("version") && featureFlag.version == "6.0", res);

MongoRunner.stopMongod(conn);
}());
