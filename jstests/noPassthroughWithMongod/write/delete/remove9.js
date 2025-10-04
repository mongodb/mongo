let t = db.jstests_remove9;
t.drop();

let js =
    "while( 1 ) { for( i = 0; i < 10000; ++i ) { db.jstests_remove9.save( {i:i} ); } db.jstests_remove9.remove( {i: {$gte:0} } ); }";
let pid = startMongoProgramNoConnect("mongo", "--eval", js, db ? db.getMongo().host : null);

Random.setRandomSeed();
for (let i = 0; i < 10000; ++i) {
    assert.commandWorked(t.remove({i: Random.randInt(10000)}));
}

stopMongoProgramByPid(pid);
