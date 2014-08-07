
t = db.explain1;
t.drop();

for ( var i=0; i<100; i++ ){
    t.save( { x : i } );
}

q = { x : { $gt : 50 } };

assert.eq( 49 , t.find( q ).count() , "A" );
assert.eq( 49 , t.find( q ).itcount() , "B" );
assert.eq( 20 , t.find( q ).limit(20).itcount() , "C" );

t.ensureIndex( { x : 1 } );

assert.eq( 49 , t.find( q ).count() , "D" );
assert.eq( 49 , t.find( q ).itcount() , "E" );
assert.eq( 20 , t.find( q ).limit(20).itcount() , "F" );

assert.eq( 49 , t.find(q).explain().n , "G" );
assert.eq( 20 , t.find(q).limit(20).explain().n , "H" );
assert.eq( 20 , t.find(q).limit(-20).explain().n , "I" );
assert.eq( 49 , t.find(q).batchSize(20).explain().n , "J" );

// verbose explain output with stats
// display index bounds

var explainGt = t.find({x: {$gt: 5}}).explain(true);
var boundsVerboseGt = explainGt.stats.inputStage.indexBounds;

print('explain stats for $gt = ' + tojson(explainGt.stats));

var explainGte = t.find({x: {$gte: 5}}).explain(true);
var boundsVerboseGte = explainGte.stats.inputStage.indexBounds;

print('explain stats for $gte = ' + tojson(explainGte.stats));

print('index bounds for $gt  = ' + tojson(explainGt.indexBounds));
print('index bounds for $gte = ' + tojson(explainGte.indexBounds));

print('verbose bounds for $gt  = ' + tojson(boundsVerboseGt));
print('verbose bounds for $gte = ' + tojson(boundsVerboseGte));

// Since the verbose bounds are opaque, all we try to confirm is that the
// verbose bounds for $gt is different from those generated for $gte.
assert.neq(boundsVerboseGt, boundsVerboseGte,
           'verbose bounds for $gt and $gte should not be the same');
