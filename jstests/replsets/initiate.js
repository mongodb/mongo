/*
 * Sanity check that initializing will fail with bad input. There are C++ unit tests for most bad
 * configs, so this is just seeing if it fails when it's supposed to.
 * @tags: [multiversion_incompatible]
 */
(function() {
"use strict";
var replTest = new ReplSetTest({name: 'testSet2', nodes: 1});
var nodes = replTest.startSet();

assert.soon(function() {
    try {
        var result = nodes[0].getDB("admin").runCommand(
            {replSetInitiate: {_id: "testSet2", members: [{_id: 0}]}});
        printjson(result);
        return (
            result.errmsg.match(/BSON field 'MemberConfig.host' is missing but a required field/));
    } catch (e) {
        print(e);
    }
    return false;
});

replTest.stopSet();
}());
