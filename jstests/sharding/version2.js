(function() {
    'use strict';

    var s = new ShardingTest({name: "version2", shards: 1});

    assert.commandWorked(s.s0.adminCommand({enablesharding: "alleyinsider"}));
    assert.commandWorked(s.s0.adminCommand({shardcollection: "alleyinsider.foo", key: {num: 1}}));
    assert.commandWorked(s.s0.adminCommand({shardcollection: "alleyinsider.bar", key: {num: 1}}));

    var a = s._connections[0].getDB("admin");

    // Setup from one client
    assert.eq(a.runCommand({"getShardVersion": "alleyinsider.foo", configdb: s._configDB}).mine.i,
              0);
    assert.eq(a.runCommand({"getShardVersion": "alleyinsider.foo", configdb: s._configDB}).global.i,
              0);

    var fooEpoch = s.getDB('config').chunks.findOne({ns: 'alleyinsider.foo'}).lastmodEpoch;
    assert.commandWorked(a.runCommand({
        setShardVersion: "alleyinsider.foo",
        configdb: s._configDB,
        authoritative: true,
        version: new Timestamp(1, 0),
        versionEpoch: fooEpoch,
        shard: "shard0000",
        shardHost: s.s.host,
    }));

    printjson(s.config.chunks.findOne());

    assert.eq(a.runCommand({"getShardVersion": "alleyinsider.foo", configdb: s._configDB}).mine.t,
              1);
    assert.eq(a.runCommand({"getShardVersion": "alleyinsider.foo", configdb: s._configDB}).global.t,
              1);

    // From a different client
    var a2 = connect(s._connections[0].name + "/admin");

    assert.eq(
        a2.runCommand({"getShardVersion": "alleyinsider.foo", configdb: s._configDB}).global.t,
        1,
        "a2 global 1");
    assert.eq(a2.runCommand({"getShardVersion": "alleyinsider.foo", configdb: s._configDB}).mine.i,
              0,
              "a2 mine 1");

    function simpleFindOne() {
        return a2.getMongo().getDB("alleyinsider").foo.findOne();
    }

    var barEpoch = s.getDB('config').chunks.findOne({ns: 'alleyinsider.bar'}).lastmodEpoch;
    assert.commandWorked(a2.runCommand({
        setShardVersion: "alleyinsider.bar",
        configdb: s._configDB,
        version: new Timestamp(1, 0),
        versionEpoch: barEpoch,
        shard: 'shard0000',
        authoritative: true
    }),
                         "setShardVersion bar temp");

    assert.throws(simpleFindOne, [], "should complain about not in sharded mode 1");

    // the only way that setSharVersion passes is if the shard agrees with the version
    // the shard takes its version from config directly
    // TODO bump timestamps in config
    // assert(a2.runCommand({ "setShardVersion": "alleyinsider.foo", configdb: s._configDB, version:
    // 2 }).ok == 1, "setShardVersion a2-1");

    // simpleFindOne(); // now should run ok

    // assert(a2.runCommand({ "setShardVersion": "alleyinsider.foo", configdb: s._configDB, version:
    // 3 }).ok == 1, "setShardVersion a2-2");

    // simpleFindOne(); // newer version is ok

    s.stop();

})();
