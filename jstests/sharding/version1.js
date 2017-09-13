(function() {

    var s = new ShardingTest({name: "version1", shards: 1});

    s.adminCommand({enablesharding: "alleyinsider"});
    s.adminCommand({shardcollection: "alleyinsider.foo", key: {num: 1}});

    // alleyinsider.foo is supposed to have one chunk, version 1|0, in shard000
    s.printShardingStatus();

    a = s._connections[0].getDB("admin");

    assert.commandFailed(
        a.runCommand({setShardVersion: "alleyinsider.foo", configdb: s._configDB}));

    assert.commandFailed(
        a.runCommand({setShardVersion: "alleyinsider.foo", configdb: s._configDB, version: "a"}));

    assert.commandFailed(a.runCommand(
        {setShardVersion: "alleyinsider.foo", configdb: s._configDB, authoritative: true}));

    assert.commandFailed(a.runCommand({
        setShardVersion: "alleyinsider.foo",
        configdb: s._configDB,
        version: new Timestamp(2, 0)
    }),
                         "should have failed b/c no auth");

    assert.commandFailed(a.runCommand({
        setShardVersion: "alleyinsider.foo",
        configdb: s._configDB,
        version: new Timestamp(2, 0),
        authoritative: true
    }),
                         "should have failed because first setShardVersion needs shard info");

    assert.commandFailed(a.runCommand({
        setShardVersion: "alleyinsider.foo",
        configdb: s._configDB,
        version: new Timestamp(2, 0),
        authoritative: true,
        shard: "shard0000",
        shardHost: s.s.host
    }),
                         "should have failed because version is config is 1|0");

    var epoch = s.getDB('config').chunks.findOne().lastmodEpoch;
    assert.commandWorked(a.runCommand({
        setShardVersion: "alleyinsider.foo",
        configdb: s._configDB,
        version: new Timestamp(1, 0),
        versionEpoch: epoch,
        authoritative: true,
        shard: "shard0000",
        shardHost: s.s.host
    }),
                         "should have worked");

    assert.commandFailed(a.runCommand({
        setShardVersion: "alleyinsider.foo",
        configdb: "a",
        version: new Timestamp(0, 2),
        versionEpoch: epoch
    }));

    assert.commandFailed(a.runCommand({
        setShardVersion: "alleyinsider.foo",
        configdb: s._configDB,
        version: new Timestamp(0, 2),
        versionEpoch: epoch
    }));

    assert.commandFailed(a.runCommand({
        setShardVersion: "alleyinsider.foo",
        configdb: s._configDB,
        version: new Timestamp(0, 1),
        versionEpoch: epoch
    }));

    // the only way that setSharVersion passes is if the shard agrees with the version
    // the shard takes its version from config directly
    // TODO bump timestamps in config
    // assert.eq( a.runCommand( { "setShardVersion" : "alleyinsider.foo" , configdb : s._configDB ,
    // version : 3 } ).oldVersion.i , 2 , "oldVersion" );

    // assert.eq( a.runCommand( { "getShardVersion" : "alleyinsider.foo" } ).mine.i , 3 , "my get
    // version A" );
    // assert.eq( a.runCommand( { "getShardVersion" : "alleyinsider.foo" } ).global.i , 3 , "my get
    // version B" );

    s.stop();

})();
