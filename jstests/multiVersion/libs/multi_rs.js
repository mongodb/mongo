//
// Utility functions for multi-version replica sets
//

/**
 * @param options {Object} see ReplSetTest.start & MongoRunner.runMongod.
 * @param user {string} optional, user name for authentication.
 * @param pwd {string} optional, password for authentication. Must be set if user is set.
 */
ReplSetTest.prototype.upgradeSet = function(options, user, pwd) {
    options = options || {};

    var primary = this.getPrimary();

    // Upgrade secondaries first
    var nodesToUpgrade = this.getSecondaries();

    // Then upgrade primaries
    nodesToUpgrade.push(primary);

    // We can upgrade with no primary downtime if we have enough nodes
    var noDowntimePossible = this.nodes.length > 2;

    for (var i = 0; i < nodesToUpgrade.length; i++) {
        var node = nodesToUpgrade[i];
        if (node == primary) {
            node = this.stepdown(node);
            this.waitForState(node, ReplSetTest.State.SECONDARY);
            primary = this.getPrimary();
        }

        var prevPrimaryId = this.getNodeId(primary);
        // merge new options into node settings...
        for (var nodeName in this.nodeOptions) {
            this.nodeOptions[nodeName] = Object.merge(this.nodeOptions[nodeName], options);
        }

        this.upgradeNode(node, options, user, pwd);

        if (noDowntimePossible)
            assert.eq(this.getNodeId(primary), prevPrimaryId);
    }
};

ReplSetTest.prototype.upgradeNode = function(node, opts, user, pwd) {
    if (user != undefined) {
        assert.eq(1, node.getDB("admin").auth(user, pwd));
    }
    jsTest.authenticate(node);

    var isMaster = node.getDB('admin').runCommand({isMaster: 1});

    if (!isMaster.arbiterOnly) {
        // Must retry this command, as it might return "currently running for election" and fail.
        // Node might still be running for an election that will fail because it lost the election
        // race with another node, at test initialization.  See SERVER-23133.
        assert.soon(function() {
            return (node.adminCommand("replSetMaintenance").ok);
        });
        this.waitForState(node, ReplSetTest.State.RECOVERING);
    }

    var newNode = this.restart(node, opts);
    if (user != undefined) {
        newNode.getDB("admin").auth(user, pwd);
    }

    var waitForStates =
        [ReplSetTest.State.PRIMARY, ReplSetTest.State.SECONDARY, ReplSetTest.State.ARBITER];
    this.waitForState(newNode, waitForStates);

    return newNode;
};

ReplSetTest.prototype.stepdown = function(nodeId) {
    nodeId = this.getNodeId(nodeId);
    assert.eq(this.getNodeId(this.getPrimary()), nodeId);
    var node = this.nodes[nodeId];

    try {
        node.getDB("admin").runCommand({replSetStepDown: 300, secondaryCatchUpPeriodSecs: 60});
        assert(false);
    } catch (ex) {
        print('Caught exception after stepDown cmd: ' + tojson(ex));
    }

    return this.reconnect(node);
};

ReplSetTest.prototype.reconnect = function(node) {
    var nodeId = this.getNodeId(node);
    this.nodes[nodeId] = new Mongo(node.host);
    var except = {};
    for (var i in node) {
        if (typeof(node[i]) == "function")
            continue;
        this.nodes[nodeId][i] = node[i];
    }

    return this.nodes[nodeId];
};

ReplSetTest.prototype.conf = function() {
    var admin = this.getPrimary().getDB('admin');

    var resp = admin.runCommand({replSetGetConfig: 1});

    if (resp.ok && !(resp.errmsg) && resp.config)
        return resp.config;

    else if (resp.errmsg && resp.errmsg.startsWith("no such cmd"))
        return admin.getSisterDB("local").system.replset.findOne();

    throw new Error("Could not retrieve replica set config: " + tojson(resp));
};
