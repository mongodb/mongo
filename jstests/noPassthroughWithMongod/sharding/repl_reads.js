// Test that doing secondaryOk reads from secondaries hits all the secondaries evenly
// @tags: [requires_sharding]

import {ShardingTest} from "jstests/libs/shardingtest.js";

function testReadLoadBalancing(numReplicas) {
    let s = new ShardingTest({shards: {rs0: {nodes: numReplicas}}, verbose: 2, other: {chunkSize: 1}});

    s.adminCommand({enablesharding: "test"});
    s.config.settings.find().forEach(printjson);

    s.adminCommand({shardcollection: "test.foo", key: {_id: 1}});

    s.getDB("test").foo.insert({a: 123});

    let primary = s.rs0.getPrimary();
    let secondaries = s.rs0.getSecondaries();

    function rsStats() {
        return s.getDB("admin").runCommand("connPoolStats")["replicaSets"][s.rs0.name];
    }

    assert.eq(numReplicas, rsStats().hosts.length);

    function isPrimaryOrSecondary(info) {
        if (!info.ok) return false;
        if (info.ismaster) return true;
        return info.secondary && !info.hidden;
    }

    assert.soon(function () {
        let x = rsStats().hosts;
        printjson(x);
        for (let i = 0; i < x.length; i++) if (!isPrimaryOrSecondary(x[i])) return false;
        return true;
    });

    for (var i = 0; i < secondaries.length; i++) {
        assert.soon(function () {
            return secondaries[i].getDB("test").foo.count() > 0;
        });
        secondaries[i].getDB("test").setProfilingLevel(2);
    }
    // Primary may change with reconfig
    primary.getDB("test").setProfilingLevel(2);

    // Store references to the connection so they won't be garbage collected.
    let connections = [];

    for (var i = 0; i < secondaries.length * 10; i++) {
        let conn = new Mongo(s._mongos[0].host);
        conn.setSecondaryOk();
        conn.getDB("test").foo.findOne();
        connections.push(conn);
    }

    let profileCriteria = {op: "query", ns: "test.foo"};

    for (var i = 0; i < secondaries.length; i++) {
        var profileCollection = secondaries[i].getDB("test").system.profile;
        assert.eq(
            10,
            profileCollection.find(profileCriteria).count(),
            "Wrong number of read queries sent to secondary " + i + " " + tojson(profileCollection.find().toArray()),
        );
    }

    const db = primary.getDB("test");

    printjson(rs.status());
    let c = rs.conf();
    print("config before: " + tojson(c));
    for (i = 0; i < c.members.length; i++) {
        if (c.members[i].host == db.runCommand("hello").primary) continue;
        c.members[i].hidden = true;
        c.members[i].priority = 0;
        break;
    }
    rs.reconfig(c);
    print("config after: " + tojson(rs.conf()));

    assert.soon(
        function () {
            let x = rsStats();
            printjson(x);
            let numOk = 0;
            // Now wait until the host disappears, since now we actually update our
            // replica sets via isMaster in mongos
            if (x.hosts.length == c["members"].length - 1) return true;
            /*
        for ( var i=0; i<x.hosts.length; i++ )
            if ( x.hosts[i].hidden )
                return true;
        */
            return false;
        },
        "one secondary not ok",
        180000,
        5000,
    );

    // Secondaries may change here
    secondaries = s.rs0.getSecondaries();

    for (var i = 0; i < secondaries.length * 10; i++) {
        let conn = new Mongo(s._mongos[0].host);
        conn.setSecondaryOk();
        conn.getDB("test").foo.findOne();
        connections.push(conn);
    }

    let counts = [];
    for (var i = 0; i < secondaries.length; i++) {
        var profileCollection = secondaries[i].getDB("test").system.profile;
        counts.push(profileCollection.find(profileCriteria).count());
    }

    counts = counts.sort();
    assert.eq(20, Math.abs(counts[1] - counts[0]), "counts wrong: " + tojson(counts));

    s.stop();
}

// for (var i = 1; i < 10; i++) {
//    testReadLoadBalancing(i)
//}

// Is there a way that this can be run multiple times with different values?
// Disabled until SERVER-11956 is solved
// testReadLoadBalancing(3)
