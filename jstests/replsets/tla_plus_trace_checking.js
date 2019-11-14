/**
 * This test enables TLA+ Trace Checking and ensures that it succeeds.
 */

(function() {
"use strict";
load("jstests/libs/check_log.js");

const failpointData = {
    mode: 'alwaysOn',
    data: {specs: ["RaftMongo"]}
};

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            logComponentVerbosity: tojsononeline({tlaPlusTrace: 2}),
            "failpoint.logForTLAPlusSpecs": tojsononeline(failpointData),
        }
    }
});

rst.startSet();
rst.initiate();

const coll = rst.getPrimary().getDB('test').collection;

jsTestLog("Insert one document");
assert.commandWorked(coll.insert({_id: 0}, {writeConcern: {w: "majority"}}));

checkLog.contains(rst.getSecondary(), '{ spec: \"RaftMongo\", action: \"AppendOplog\",');

rst.stopSet();
})();
