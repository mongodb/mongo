//
// Utility functions for multi-version replica sets
//

/**
 * Upgrade or downgrade replica sets.
 *
 * @param options {Object} see ReplSetTest.start & MongoRunner.runMongod.
 * @param user {string} optional, user name for authentication.
 * @param pwd {string} optional, password for authentication. Must be set if user is set.
 */
ReplSetTest.prototype.upgradeSet = function(options, user, pwd) {
    let primary = this.getPrimary();

    this.upgradeSecondaries(primary, Object.assign({}, options), user, pwd);
    this.upgradeArbiters(primary, Object.assign({}, options), user, pwd);

    // Upgrade the primary after stepping down.
    this.upgradePrimary(primary, Object.assign({}, options), user, pwd);
};

function mergeNodeOptions(nodeOptions, options) {
    for (let nodeName in nodeOptions) {
        nodeOptions[nodeName] = Object.merge(nodeOptions[nodeName], options);
    }
    return nodeOptions;
}

ReplSetTest.prototype.upgradeMembers = function(primary, members, options, user, pwd) {
    const noDowntimePossible = this.nodes.length > 2;

    // Merge new options into node settings.
    this.nodeOptions = mergeNodeOptions(this.nodeOptions, options);

    for (let member of members) {
        this.upgradeNode(member, options, user, pwd);

        if (noDowntimePossible)
            assert.eq(this.getPrimary(), primary);
    }
};

ReplSetTest.prototype.getNonArbiterSecondaries = function() {
    let secs = this.getSecondaries();
    let arbiters = this.getArbiters();
    let nonArbiters = secs.filter(x => !arbiters.includes(x));
    return nonArbiters;
};

ReplSetTest.prototype.upgradeSecondaries = function(primary, options, user, pwd) {
    this.upgradeMembers(primary, this.getNonArbiterSecondaries(), options, user, pwd);
};

ReplSetTest.prototype.upgradeArbiters = function(primary, options, user, pwd) {
    // We don't support downgrading data files for arbiters. We need to instead delete the dbpath.
    const oldStartClean = {startClean: (options && !!options["startClean"])};
    if (options && options.binVersion == "last-stable") {
        options["startClean"] = true;
    }
    this.upgradeMembers(primary, this.getArbiters(), options, user, pwd);
    // Make sure we don't set {startClean:true} on other nodes unless the user explicitly requested.
    this.nodeOptions = mergeNodeOptions(this.nodeOptions, oldStartClean);
};

ReplSetTest.prototype.upgradePrimary = function(primary, options, user, pwd) {
    const noDowntimePossible = this.nodes.length > 2;

    // Merge new options into node settings.
    this.nodeOptions = mergeNodeOptions(this.nodeOptions, options);

    let oldPrimary = this.stepdown(primary);
    this.waitForState(oldPrimary, ReplSetTest.State.SECONDARY);

    // stepping down the node can close the connection and lose the authentication state, so
    // re-authenticate here before calling awaitNodesAgreeOnPrimary().
    if (user != undefined) {
        oldPrimary.getDB('admin').auth(user, pwd);
    }
    jsTest.authenticate(oldPrimary);

    this.awaitNodesAgreeOnPrimary();
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
    jsTest.authenticate(node);

    var isMaster = node.getDB('admin').runCommand({isMaster: 1});

    if (!isMaster.arbiterOnly) {
        // Must retry this command, as it might return "currently running for election" and fail.
        // Node might still be running for an election that will fail because it lost the election
        // race with another node, at test initialization.  See SERVER-23133.
        assert.soonNoExcept(function() {
            assert.commandWorked(node.adminCommand("replSetMaintenance"));
            return true;
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

    assert.soonNoExcept(function() {
        // Due to a rare race condition in stepdown, it's possible the secondary just replicated
        // the most recent write and sent replSetUpdatePosition to the primary, and that
        // replSetUpdatePosition command gets interrupted by the stepdown.  In that case,
        // the secondary will clear its sync source, but will be unable to re-connect to the
        // primary that is trying to step down, because they are at the same OpTime.  The primary
        // will then get stuck waiting forever for the secondary to catch up so it can complete
        // stepdown.  Adding a garbage write here ensures that the secondary will be able to
        // resume syncing from the primary in this case, which in turn will let the primary
        // finish stepping down successfully.
        node.getDB('admin').garbageWriteToAdvanceOpTime.insert({a: 1});
        assert.adminCommandWorkedAllowingNetworkError(
            node, {replSetStepDown: 5 * 60, secondaryCatchUpPeriodSecs: 60});
        return true;
    });

    return this.reconnect(node);
};

ReplSetTest.prototype.reconnect = function(node) {
    var nodeId = this.getNodeId(node);
    this.nodes[nodeId] = new Mongo(node.host);
    for (var i in node) {
        if (typeof (node[i]) == "function")
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
