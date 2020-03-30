// Use TestOnly command replSetTestEgress to connect to members.

(function() {
'use strict';

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

const admin = rst.getPrimary().getDB('admin');

jsTest.log('Connecting to any host');
const anyHost = assert.commandWorked(admin.runCommand({replSetTestEgress: 1}));
jsTest.log('Connected to ' + anyHost.target);

rst.nodeList().forEach(function(host) {
    jsTest.log('Connecting to specific host: ' + host);
    const node = assert.commandWorked(admin.runCommand({replSetTestEgress: 1, target: host}));
    jsTest.log('Connected to specific host: ' + node.target);
    assert.eq(node.target, host);
});

rst.stopSet();
}());
