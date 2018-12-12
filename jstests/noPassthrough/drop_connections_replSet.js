(function() {
    "use strict";

    const rst = new ReplSetTest({nodes: 3});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    rst.awaitSecondaryNodes();

    var cfg = primary.getDB('local').system.replset.findOne();
    var memberHost = cfg.members[2].host;
    var removedMember = cfg.members.splice(2, 1);
    cfg.version++;

    primary.adminCommand({replSetReconfig: cfg});

    assert.eq(1, primary.adminCommand({connPoolStats: 1}).hosts[memberHost].available);
    assert.commandWorked(primary.adminCommand({dropConnections: 1, hostAndPort: [memberHost]}));
    assert.eq(0, primary.adminCommand({connPoolStats: 1}).hosts[memberHost].available);

    // need to re-add removed node or test complain about the replset config
    cfg.members.push(removedMember[0]);
    cfg.version++;
    primary.adminCommand({replSetReconfig: cfg});

    rst.stopSet();
})();
