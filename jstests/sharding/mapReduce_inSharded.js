(function() {
    "use strict";

    var verifyOutput = function(out) {
        printjson(out);
        assert.commandWorked(out);
        assert.eq(out.counts.input, 51200, "input count is wrong");
        assert.eq(out.counts.emit, 51200, "emit count is wrong");
        assert.gt(out.counts.reduce, 99, "reduce count is wrong");
        assert.eq(out.counts.output, 512, "output count is wrong");
    };

    var st = new ShardingTest(
        {shards: 2, verbose: 1, mongos: 1, other: {chunkSize: 1, enableBalancer: true}});

    st.adminCommand({enablesharding: "mrShard"});
    st.ensurePrimaryShard('mrShard', 'shard0001');
    st.adminCommand({shardcollection: "mrShard.srcSharded", key: {"_id": 1}});

    var db = st.getDB("mrShard");

    var bulk = db.srcSharded.initializeUnorderedBulkOp();
    for (var j = 0; j < 100; j++) {
        for (var i = 0; i < 512; i++) {
            bulk.insert({j: j, i: i});
        }
    }
    assert.writeOK(bulk.execute());

    function map() {
        emit(this.i, 1);
    }
    function reduce(key, values) {
        return Array.sum(values);
    }

    // sharded src
    var suffix = "InSharded";

    var out = db.srcSharded.mapReduce(map, reduce, "mrBasic" + suffix);
    verifyOutput(out);

    out = db.srcSharded.mapReduce(map, reduce, {out: {replace: "mrReplace" + suffix}});
    verifyOutput(out);

    out = db.srcSharded.mapReduce(map, reduce, {out: {merge: "mrMerge" + suffix}});
    verifyOutput(out);

    out = db.srcSharded.mapReduce(map, reduce, {out: {reduce: "mrReduce" + suffix}});
    verifyOutput(out);

    out = db.srcSharded.mapReduce(map, reduce, {out: {inline: 1}});
    verifyOutput(out);
    assert(out.results != 'undefined', "no results for inline");

    // Ensure that mapReduce with a sharded input collection can accept the collation option.
    out = db.srcSharded.mapReduce(map, reduce, {out: {inline: 1}, collation: {locale: "en_US"}});
    verifyOutput(out);
    assert(out.results != 'undefined', "no results for inline with collation");

    out = db.srcSharded.mapReduce(
        map, reduce, {out: {replace: "mrReplace" + suffix, db: "mrShardOtherDB"}});
    verifyOutput(out);

    out = db.runCommand({
        mapReduce: "srcSharded",  // use new name mapReduce rather than mapreduce
        map: map,
        reduce: reduce,
        out: "mrBasic" + "srcSharded",
    });
    verifyOutput(out);

    // Ensure that the collation option is propagated to the shards. This uses a case-insensitive
    // collation, and the query seeding the mapReduce should only match the document if the
    // collation is passed along to the shards.
    assert.writeOK(db.srcSharded.remove({}));
    assert.eq(db.srcSharded.find().itcount(), 0);
    assert.writeOK(db.srcSharded.insert({i: 0, j: 0, str: "FOO"}));
    out = db.srcSharded.mapReduce(
        map,
        reduce,
        {out: {inline: 1}, query: {str: "foo"}, collation: {locale: "en_US", strength: 2}});
    assert.commandWorked(out);
    assert.eq(out.counts.input, 1);
})();
