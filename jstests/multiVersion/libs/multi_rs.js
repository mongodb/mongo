//
// Utility functions for multi-version replica sets
//

/**
 * @param options {Object} see ReplSetTest.start & MongoRunner.runMongod.
 * @param user {string} optional, user name for authentication.
 * @param pwd {string} optional, password for authentication. Must be set if user is set.
 */
ReplSetTest.prototype.upgradeSet = function(options, user, pwd) {
    let primary = this.getPrimary();

    // Upgrade secondaries first.
    this.upgradeSecondaries(primary, options, user, pwd);

    // Then upgrade the primary after stepping down.
    this.upgradePrimary(primary, options, user, pwd);

};

ReplSetTest.prototype.upgradeSecondaries = function(primary, options, user, pwd) {
    const noDowntimePossible = this.nodes.length > 2;

    // Merge new options into node settings.
    for (let nodeName in this.nodeOptions) {
        this.nodeOptions[nodeName] = Object.merge(this.nodeOptions[nodeName], options);
    }

    for (let secondary of this.getSecondaries()) {
        this.upgradeNode(secondary, options, user, pwd);

        if (noDowntimePossible)
            assert.eq(this.getPrimary(), primary);
    }
};

ReplSetTest.prototype.upgradePrimary = function(primary, options, user, pwd) {
    const noDowntimePossible = this.nodes.length > 2;

    // Merge new options into node settings.
    for (let nodeName in this.nodeOptions) {
        this.nodeOptions[nodeName] = Object.merge(this.nodeOptions[nodeName], options);
    }

    let oldPrimary = this.stepdown(primary);
    primary = this.getPrimary();

    this.upgradeNode(oldPrimary, options, user, pwd);

    let newPrimary = this.getPrimary();

    if (noDowntimePossible)
        assert.eq(newPrimary, primary);

    return newPrimary;
};

ReplSetTest.prototype.upgradeNode = function(node, opts = {}, user, pwd) {
    if (user != undefined) {
        assert.eq(1, node.getDB("admin").auth(user, pwd));
    }

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
