// test that reconfigging only tag changes is properly reflected in hello

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
var hello = newConn.adminCommand({hello: 1});
assert(hello.tags != null, 'hello: ' + tojson(hello));

print('success: ' + tojson(hello));
replTest.stopSet();
