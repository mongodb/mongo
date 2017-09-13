// test that reconfigging only tag changes is properly reflected in isMaster

var replTest = new ReplSetTest({nodes: 2});
replTest.startSet({oplogSize: 10});
replTest.initiate();
replTest.awaitSecondaryNodes();

// Tag primary with { dc: 'ny', tag: 'one' }, secondary with { dc: 'ny', tag: 'two' }
var primary = replTest.getPrimary();
var secondary = replTest.getSecondary();
var rsConfig = primary.getDB("local").system.replset.findOne();
jsTest.log('got rsconf ' + tojson(rsConfig));
rsConfig.members.forEach(function(member) {
    if (member.host == primary.host) {
        member.tags = {dc: 'ny', tag: 'one'};
    } else {
        member.tags = {dc: 'ny', tag: 'two'};
    }
});

rsConfig.version++;

jsTest.log('new rsconf ' + tojson(rsConfig));

try {
    var res = primary.adminCommand({replSetReconfig: rsConfig});
    jsTest.log('reconfig res: ' + tojson(res));  // Should not see this
} catch (e) {
    jsTest.log('replSetReconfig error: ' + e);
}

replTest.awaitSecondaryNodes();

var testDB = primary.getDB('test');

var newConn = new Mongo(primary.host);
var isMaster = newConn.adminCommand({isMaster: 1});
assert(isMaster.tags != null, 'isMaster: ' + tojson(isMaster));

print('success: ' + tojson(isMaster));
replTest.stopSet();
