// server-9444 support disk storage of intermediate results in aggregation
(function() {
    'use strict';

    var t = db.server9444;
    t.drop();

    var sharded = (typeof(RUNNING_IN_SHARDED_AGG_TEST) != 'undefined');  // see end of testshard1.js
    if (sharded) {
        assert.commandWorked(
            db.adminCommand({shardcollection: t.getFullName(), key: {"_id": 'hashed'}}));
    }

    var memoryLimitMB = sharded ? 200 : 100;

    function loadData() {
        var bigStr = Array(1024 * 1024 + 1).toString();  // 1MB of ','
        for (var i = 0; i < memoryLimitMB + 1; i++)
            t.insert({_id: i, bigStr: i + bigStr, random: Math.random()});

        assert.gt(t.stats().size, memoryLimitMB * 1024 * 1024);
    }
    loadData();

    function test(pipeline, outOfMemoryCode) {
        // ensure by default we error out if exceeding memory limit
        var res = t.runCommand('aggregate', {pipeline: pipeline});
        assert.commandFailed(res);
        assert.eq(res.code, outOfMemoryCode);

        // ensure allowDiskUse: false does what it says
        res = t.runCommand('aggregate', {pipeline: pipeline, allowDiskUse: false});
        assert.commandFailed(res);
        assert.eq(res.code, outOfMemoryCode);

        // allowDiskUse only supports bool. In particular, numbers aren't allowed.
        res = t.runCommand('aggregate', {pipeline: pipeline, allowDiskUse: 1});
        assert.commandFailed(res);

        // ensure we work when allowDiskUse === true
        res = t.aggregate(pipeline, {allowDiskUse: true});
        assert.eq(res.itcount(), t.count());  // all tests output one doc per input doc
    }

    var groupCode = 16945;
    var sortCode = 16819;
    var sortLimitCode = 16820;

    test([{$group: {_id: '$_id', bigStr: {$first: '$bigStr'}}}], groupCode);

    // sorting with _id would use index which doesn't require extsort
    test([{$sort: {random: 1}}], sortCode);
    test([{$sort: {bigStr: 1}}], sortCode);  // big key and value

    // make sure sort + large limit won't crash the server (SERVER-10136)
    test([{$sort: {bigStr: 1}}, {$limit: 1000 * 1000 * 1000}], sortLimitCode);

    // test combining two extSorts in both same and different orders
    test([{$group: {_id: '$_id', bigStr: {$first: '$bigStr'}}}, {$sort: {_id: 1}}], groupCode);
    test([{$group: {_id: '$_id', bigStr: {$first: '$bigStr'}}}, {$sort: {_id: -1}}], groupCode);
    test([{$group: {_id: '$_id', bigStr: {$first: '$bigStr'}}}, {$sort: {random: 1}}], groupCode);
    test([{$sort: {random: 1}}, {$group: {_id: '$_id', bigStr: {$first: '$bigStr'}}}], sortCode);

    var origDB = db;
    if (sharded) {
        // Stop balancer first before dropping so there will be no contention on the ns lock.
        // It's alright to modify the global db variable since sharding tests never run in parallel.
        db = db.getSiblingDB('config');
        sh.stopBalancer();
    }

    // don't leave large collection laying around
    t.drop();

    if (sharded) {
        sh.startBalancer();
        db = origDB;
    }
})();
