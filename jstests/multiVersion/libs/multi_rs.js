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

    var isMaster = node.getDB('admin').runCommand({isMaster: 1});

    if (!isMaster.arbiterOnly) {
        assert.commandWorked(node.adminCommand("replSetMaintenance"));
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
        node.getDB("admin").runCommand({replSetStepDown: 50, force: true});
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
