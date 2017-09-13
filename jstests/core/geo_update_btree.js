// Tests whether the geospatial search is stable under btree updates

var coll = db.getCollection("jstests_geo_update_btree");
coll.drop();

coll.ensureIndex({loc: '2d'});

var big = new Array(3000).toString();

if (testingReplication) {
    coll.setWriteConcern({w: 2});
}

Random.setRandomSeed();

var parallelInsert = startParallelShell(
    "Random.setRandomSeed();" + "for ( var i = 0; i < 1000; i++ ) {" +
    "    var doc = { loc: [ Random.rand() * 180, Random.rand() * 180 ], v: '' };" +
    "    db.jstests_geo_update_btree.insert(doc);" + "}");

for (i = 0; i < 1000; i++) {
    coll.update({
        loc:
            {$within: {$center: [[Random.rand() * 180, Random.rand() * 180], Random.rand() * 50]}}
    },
                {$set: {v: big}},
                false,
                true);

    if (i % 10 == 0)
        print(i);
}

parallelInsert();
