(function() {

    jsTest.log('Testing that dump utilizes read preferences and tags');

    var toolTest = new ToolTest('dump_preference_and_tags');

    var replset1 = new ReplSetTest({nodes:3, name:'replset'});

    replset1.startSet();

    replset1.initiate();

    var primary = replset1.getPrimary();

    var secondaries = replset1.getSecondaries();

    secondaries.forEach(function(secondary){
        secondary.getDB('foo').setProfilingLevel(2)
    })
    primary.getDB('foo').setProfilingLevel(2)

    // rs functions actually operate on db
    db = primary.getDB('foo');

    var conf = rs.conf();

    var hostByTag = {};
    var i = 1;
    conf.members.forEach(function(member){
        if (member.host == primary.host) {
            member.tags = { use: "primary" };
        } else {
            member.tags = { use: "secondary" + i };
            hostByTag["secondary"+i]=member.host;
            i++;
        }
    })

    rs.reconfig(conf);

    runMongoProgram('mongodump', '--host', "replset/"+primary.host, '--readPreference=nearest', '--tags={use:"secondary1"}');
    jsTest.log("rawMongoProgramOutput "+ rawMongoProgramOutput());

    replset1.nodes.forEach(function(node){
        var count = node.getDB('foo').system.profile.find().count();
        jsTest.log(node.host+" "+count);
        if (node.host == hostByTag["secondary1"]) {
            assert.ne(count,0,node.host);
        } else {
            assert.eq(count,0,node.host);
        }
    })
})();
