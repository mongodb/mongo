
verifyOutput = function(out) {
    printjson(out);
    assert.eq(out.counts.input, 51200, "input count is wrong");
    assert.eq(out.counts.emit, 51200, "emit count is wrong");
    assert.gt(out.counts.reduce, 99, "reduce count is wrong");
    assert.eq(out.counts.output, 512, "output count is wrong");
}


s = new ShardingTest( "mrShard" , 2 , 1 , 1 , { chunksize : 1 } );

s.adminCommand( { enablesharding : "mrShard" } )
s.adminCommand( { shardcollection : "mrShard.srcSharded", key : { "_id" : 1 } } )
db = s.getDB( "mrShard" );

for (j=0; j<100; j++) for (i=0; i<512; i++){ db.srcNonSharded.save({j:j, i:i})}
for (j=0; j<100; j++) for (i=0; i<512; i++){ db.srcSharded.save({j:j, i:i})}
db.getLastError();

function map() { emit(this.i, 1); }
function reduce(key, values) { return Array.sum(values) } 

// non-sharded in/out
var suffix = "";

out = db.srcNonSharded.mapReduce(map, reduce, "mrBasic" + suffix);
verifyOutput(out);

out = db.srcNonSharded.mapReduce(map, reduce, { out: { replace: "mrReplace" + suffix } });
verifyOutput(out);

out = db.srcNonSharded.mapReduce(map, reduce, { out: { merge: "mrMerge" + suffix } });
verifyOutput(out);

out = db.srcNonSharded.mapReduce(map, reduce, { out: { reduce: "mrReduce" + suffix } });
verifyOutput(out);

out = db.srcNonSharded.mapReduce(map, reduce, { out: { inline: "mrInline" + suffix } });
verifyOutput(out);
assert(out.results != 'undefined', "no results for inline");

out = db.srcNonSharded.mapReduce(map, reduce, { out: { replace: "mrReplace" + suffix, db: "mrShardOtherDB" } });
verifyOutput(out);

// sharded src
suffix = "InSharded";

out = db.srcSharded.mapReduce(map, reduce, "mrBasic" + suffix);
verifyOutput(out);

out = db.srcSharded.mapReduce(map, reduce, { out: { replace: "mrReplace" + suffix } });
verifyOutput(out);

out = db.srcSharded.mapReduce(map, reduce, { out: { merge: "mrMerge" + suffix } });
verifyOutput(out);

out = db.srcSharded.mapReduce(map, reduce, { out: { reduce: "mrReduce" + suffix } });
verifyOutput(out);

out = db.srcSharded.mapReduce(map, reduce, { out: { inline: 1 } });
verifyOutput(out);
assert(out.results != 'undefined', "no results for inline");

out = db.srcSharded.mapReduce(map, reduce, { out: { replace: "mrReplace" + suffix, db: "mrShardOtherDB" } });
verifyOutput(out);

// sharded src sharded dst
suffix = "InShardedOutSharded";

out = db.srcSharded.mapReduce(map, reduce, { out: { replace: "mrReplace" + suffix, sharded: true } });
verifyOutput(out);

out = db.srcSharded.mapReduce(map, reduce, { out: { merge: "mrMerge" + suffix, sharded: true } });
verifyOutput(out);

out = db.srcSharded.mapReduce(map, reduce, { out: { reduce: "mrReduce" + suffix, sharded: true } });
verifyOutput(out);

out = db.srcSharded.mapReduce(map, reduce, { out: { inline: 1, sharded: true } });
verifyOutput(out);
assert(out.results != 'undefined', "no results for inline");

out = db.srcSharded.mapReduce(map, reduce, { out: { replace: "mrReplace" + suffix, db: "mrShardOtherDB", sharded: true } });
verifyOutput(out);

// non sharded src sharded dst
suffix = "OutSharded";

out = db.srcNonSharded.mapReduce(map, reduce, { out: { replace: "mrReplace" + suffix, sharded: true } });
verifyOutput(out);

out = db.srcNonSharded.mapReduce(map, reduce, { out: { merge: "mrMerge" + suffix, sharded: true } });
verifyOutput(out);

out = db.srcNonSharded.mapReduce(map, reduce, { out: { reduce: "mrReduce" + suffix, sharded: true } });
verifyOutput(out);

out = db.srcNonSharded.mapReduce(map, reduce, { out: { inline: 1, sharded: true } });
verifyOutput(out);
assert(out.results != 'undefined', "no results for inline");

out = db.srcNonSharded.mapReduce(map, reduce, { out: { replace: "mrReplace" + suffix, db: "mrShardOtherDB", sharded: true } });
verifyOutput(out);

