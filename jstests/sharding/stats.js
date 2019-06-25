(function() {

    var s = new ShardingTest({name: "stats", shards: 2, mongos: 1});

    s.adminCommand({enablesharding: "test"});

    db = s.getDB("test");
    s.ensurePrimaryShard('test', s.shard1.shardName);

    function numKeys(o) {
        var num = 0;
        for (var x in o)
            num++;
        return num;
    }

    db.foo.drop();
    //	SERVER-29678 changed collStats so versions > 4.0 now return 0s on NS not found
    if (MongoRunner.getBinVersionFor(jsTest.options().mongosBinVersion) === '4.0') {
        // TODO: This should be fixed in 4.4
        let res = db.foo.stats();
        if (res.ok === 1) {
            // Possible to hit a shard that is actually version >= 4.2 => result should be zeros
            assert(res.size === 0 && res.count === 0 && res.storageSize === 0 &&
                   res.nindexes === 0);
        } else {
            assert.commandFailed(
                db.foo.stats(),
                'db.collection.stats() should fail non-existent in versions <= 4.0');
        }
    } else {
        assert.commandWorked(db.foo.stats(),
                             'db.collection.stats() should return 0s on non-existent collection');
    }

    // ---------- load some data -----

    // need collections sharded before and after main collection for proper test
    s.adminCommand({shardcollection: "test.aaa", key: {_id: 1}});
    s.adminCommand(
        {shardcollection: "test.foo", key: {_id: 1}});  // this collection is actually used
    s.adminCommand({shardcollection: "test.zzz", key: {_id: 1}});

    N = 10000;
    s.adminCommand({split: "test.foo", middle: {_id: N / 2}});
    s.adminCommand({
        moveChunk: "test.foo",
        find: {_id: 3},
        to: s.getNonPrimaries("test")[0],
        _waitForDelete: true
    });

    var bulk = db.foo.initializeUnorderedBulkOp();
    for (i = 0; i < N; i++)
        bulk.insert({_id: i});
    assert.writeOK(bulk.execute());

    // Flush all writes to disk since some of the stats are dependent on state in disk (like
    // totalIndexSize).
    assert.commandWorked(db.adminCommand({fsync: 1}));

    a = s.shard0.getDB("test");
    b = s.shard1.getDB("test");

    x = assert.commandWorked(db.foo.stats());
    assert.eq(N, x.count, "coll total count expected");
    assert.eq(db.foo.count(), x.count, "coll total count match");
    assert.eq(2, x.nchunks, "coll chunk num");
    assert.eq(2, numKeys(x.shards), "coll shard num");
    assert.eq(
        N / 2, x.shards[s.shard0.shardName].count, "coll count on s.shard0.shardName expected");
    assert.eq(
        N / 2, x.shards[s.shard1.shardName].count, "coll count on s.shard1.shardName expected");
    assert.eq(a.foo.count(),
              x.shards[s.shard0.shardName].count,
              "coll count on s.shard0.shardName match");
    assert.eq(b.foo.count(),
              x.shards[s.shard1.shardName].count,
              "coll count on s.shard1.shardName match");
    assert(!x.shards[s.shard0.shardName].indexDetails,
           'indexDetails should not be present in s.shard0.shardName: ' +
               tojson(x.shards[s.shard0.shardName]));
    assert(!x.shards[s.shard1.shardName].indexDetails,
           'indexDetails should not be present in s.shard1.shardName: ' +
               tojson(x.shards[s.shard1.shardName]));

    a_extras = a.stats().objects - a.foo.count();
    b_extras = b.stats().objects - b.foo.count();
    print("a_extras: " + a_extras);
    print("b_extras: " + b_extras);

    x = assert.commandWorked(db.stats());

    assert.eq(N + (a_extras + b_extras), x.objects, "db total count expected");
    assert.eq(2, numKeys(x.raw), "db shard num");
    assert.eq((N / 2) + a_extras,
              x.raw[s.shard0.name].objects,
              "db count on s.shard0.shardName expected");
    assert.eq((N / 2) + b_extras,
              x.raw[s.shard1.name].objects,
              "db count on s.shard1.shardName expected");
    assert.eq(
        a.stats().objects, x.raw[s.shard0.name].objects, "db count on s.shard0.shardName match");
    assert.eq(
        b.stats().objects, x.raw[s.shard1.name].objects, "db count on s.shard1.shardName match");

    /* Test db.stat() and db.collection.stat() scaling */

    /* Helper functions */
    function statComp(stat, stat_scaled, scale) {
        /* Because of loss of floating point precision, do not check exact equality */
        if (stat == stat_scaled)
            return true;

        var msg = 'scaled: ' + stat_scaled + ', stat: ' + stat + ', scale: ' + scale;
        assert.lte((stat_scaled - 2), (stat / scale), msg);
        assert.gte((stat_scaled + 2), (stat / scale), msg);
    }

    function dbStatComp(stat_obj, stat_obj_scaled, scale) {
        statComp(stat_obj.dataSize, stat_obj_scaled.dataSize, scale);
        statComp(stat_obj.storageSize, stat_obj_scaled.storageSize, scale);
        statComp(stat_obj.indexSize, stat_obj_scaled.indexSize, scale);
        statComp(stat_obj.fileSize, stat_obj_scaled.fileSize, scale);
        /* avgObjSize not scaled.  See SERVER-7347 */
        statComp(stat_obj.avgObjSize, stat_obj_scaled.avgObjSize, 1);
    }

    function collStatComp(stat_obj, stat_obj_scaled, scale, mongos) {
        statComp(stat_obj.size, stat_obj_scaled.size, scale);
        statComp(stat_obj.storageSize, stat_obj_scaled.storageSize, scale);
        statComp(stat_obj.totalIndexSize, stat_obj_scaled.totalIndexSize, scale);
        statComp(stat_obj.totalSize, stat_obj_scaled.totalSize, scale);
        statComp(stat_obj.avgObjSize, stat_obj_scaled.avgObjSize, 1);
        /* lastExtentSize doesn't exist in mongos level collection stats */
        if (!mongos) {
            statComp(stat_obj.lastExtentSize, stat_obj_scaled.lastExtentSize, scale);
        }
    }

    /* db.stats() tests */
    db_not_scaled = assert.commandWorked(db.stats());
    db_scaled_512 = assert.commandWorked(db.stats(512));
    db_scaled_1024 = assert.commandWorked(db.stats(1024));

    for (var shard in db_not_scaled.raw) {
        dbStatComp(db_not_scaled.raw[shard], db_scaled_512.raw[shard], 512);
        dbStatComp(db_not_scaled.raw[shard], db_scaled_1024.raw[shard], 1024);
    }

    dbStatComp(db_not_scaled, db_scaled_512, 512);
    dbStatComp(db_not_scaled, db_scaled_1024, 1024);

    /* db.collection.stats() tests */
    coll_not_scaled = assert.commandWorked(db.foo.stats());
    coll_scaled_512 = assert.commandWorked(db.foo.stats(512));
    coll_scaled_1024 = assert.commandWorked(db.foo.stats(1024));

    for (var shard in coll_not_scaled.shards) {
        collStatComp(coll_not_scaled.shards[shard], coll_scaled_512.shards[shard], 512, false);
        collStatComp(coll_not_scaled.shards[shard], coll_scaled_1024.shards[shard], 1024, false);
    }

    collStatComp(coll_not_scaled, coll_scaled_512, 512, true);
    collStatComp(coll_not_scaled, coll_scaled_1024, 1024, true);

    /* db.collection.stats() - indexDetails tests */
    (function() {
        var t = db.foo;

        assert.commandWorked(t.ensureIndex({a: 1}));
        assert.eq(2, t.getIndexes().length);

        var isWiredTiger =
            (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger");

        var stats = assert.commandWorked(t.stats({indexDetails: true}));
        var shardName;
        var shardStats;
        for (shardName in stats.shards) {
            shardStats = stats.shards[shardName];
            assert(shardStats.indexDetails,
                   'indexDetails missing for ' + shardName + ': ' + tojson(shardStats));
            if (isWiredTiger) {
                assert.eq(t.getIndexes().length,
                          Object.keys(shardStats.indexDetails).length,
                          'incorrect number of entries in WiredTiger indexDetails: ' +
                              tojson(shardStats));
            }
        }

        function getIndexName(indexKey) {
            var indexes = t.getIndexes().filter(function(doc) {
                return friendlyEqual(doc.key, indexKey);
            });
            assert.eq(
                1,
                indexes.length,
                tojson(indexKey) + ' not found in getIndexes() result: ' + tojson(t.getIndexes()));
            return indexes[0].name;
        }

        function checkIndexDetails(options, indexName) {
            var stats = assert.commandWorked(t.stats(options));
            for (shardName in stats.shards) {
                shardStats = stats.shards[shardName];
                assert(shardStats.indexDetails,
                       'indexDetails missing from db.collection.stats(' + tojson(options) +
                           ').shards[' + shardName + '] result: ' + tojson(shardStats));
                // Currently, indexDetails is only supported with WiredTiger.
                if (isWiredTiger) {
                    assert.eq(1,
                              Object.keys(shardStats.indexDetails).length,
                              'WiredTiger indexDetails must have exactly one entry');
                    assert(shardStats.indexDetails[indexName],
                           indexName + ' missing from WiredTiger indexDetails: ' +
                               tojson(shardStats.indexDetails));
                    assert.neq(0,
                               Object.keys(shardStats.indexDetails[indexName]).length,
                               indexName + ' exists in indexDetails but contains no information: ' +
                                   tojson(shardStats.indexDetails));
                }
            }
        }

        // indexDetailsKey - show indexDetails results for this index key only.
        var indexKey = {a: 1};
        var indexName = getIndexName(indexKey);
        checkIndexDetails({indexDetails: true, indexDetailsKey: indexKey}, indexName);

        // indexDetailsName - show indexDetails results for this index name only.
        checkIndexDetails({indexDetails: true, indexDetailsName: indexName}, indexName);
    }());

    s.stop();
})();
