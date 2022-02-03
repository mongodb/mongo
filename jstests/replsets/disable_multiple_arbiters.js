/**
 * Verifies that multiple arbiters cannot be added without setting allowMultipleArbiters=true.
 *
 * @tags: [requires_fcv_53]
 */

function multiple_arbiters(multiple_arbiters_allowed) {
    "use strict";

    jsTestLog("multiple_arbiters(" + multiple_arbiters_allowed + ")");

    const name = "disable_multiple_arbiters";
    const rst = new ReplSetTest({name: name, nodes: 4});
    const nodes = rst.nodeList();

    let config = {};
    if (multiple_arbiters_allowed) {
        config["setParameter"] = {allowMultipleArbiters: true};
    }
    rst.startSet(config);
    rst.initiate({
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2]},
            {"_id": 3, "host": nodes[3], "arbiterOnly": true}
        ]
    });

    const arbiterConn = rst.add(config);
    const admin = rst.getPrimary().getDB("admin");
    const conf = admin.runCommand({replSetGetConfig: 1}).config;
    conf.members.push({_id: 4, host: arbiterConn.host, arbiterOnly: true});
    conf.version++;

    jsTestLog('Add second arbiter');
    const response = admin.runCommand({replSetReconfig: conf});

    if (multiple_arbiters_allowed) {
        assert.commandWorked(response);
    } else {
        assert.commandFailedWithCode(response, ErrorCodes.NewReplicaSetConfigurationIncompatible);
    }

    if (!multiple_arbiters_allowed) {
        // Remove the node since it was not successfully added to the config, so we should not run
        // validation checks on it when we shut down the replica set.
        rst.remove(4);
    }
    rst.stopSet();
}

multiple_arbiters(true);
multiple_arbiters(false);
