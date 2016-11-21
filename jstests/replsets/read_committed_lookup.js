/**
 * Tests that a $lookup and $graphLookup stage within an aggregation pipeline will read only
 * committed data if the pipeline is using a majority readConcern.
 */

load("jstests/replsets/rslib.js");           // For startSetIfSupportsReadMajority.
load("jstests/libs/read_committed_lib.js");  // For testReadCommittedLookup

(function() {
    "use strict";

    // Confirm majority readConcern works on a replica set.
    const replSetName = "lookup_read_majority";
    let rst = new ReplSetTest({
        nodes: 3,
        name: replSetName,
        nodeOptions: {
            enableMajorityReadConcern: "",
            shardsvr: "",
        }
    });

    if (!startSetIfSupportsReadMajority(rst)) {
        jsTest.log("skipping test since storage engine doesn't support committed reads");
        return;
    }

    const nodes = rst.nodeList();
    const config = {
        _id: replSetName,
        members: [
            {_id: 0, host: nodes[0]},
            {_id: 1, host: nodes[1], priority: 0},
            {_id: 2, host: nodes[2], arbiterOnly: true},
        ]
    };

    rst.initiate(config);

    let shardSecondary = rst.liveNodes.slaves[0];

    testReadCommittedLookup(rst.getPrimary().getDB("test"), shardSecondary, rst);

})();
