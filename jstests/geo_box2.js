
t = db.geo_box2;

t.drop()

for (i=1; i<10; i++) { 
    for(j=1; j<10; j++) { 
        t.insert({loc : [i,j]}); 
    } 
}

t.ensureIndex({"loc" : "2d"} )
assert.eq( 9 , t.find({loc : {$within : {$box : [[4,4],[6,6]]}}}).itcount() , "A1" );

t.dropIndex( { "loc" : "2d" } )

t.ensureIndex({"loc" : "2d"} , {"min" : 0, "max" : 10})
assert.eq( 9 , t.find({loc : {$within : {$box : [[4,4],[6,6]]}}}).itcount() , "B1" );

// 'indexBounds.loc' in explain output should be filled in with at least
// one bounding box.
// Actual values is dependent on implementation of 2d execution stage.
var explain = t.find({loc : {$within : {$box : [[4,4],[6,6]]}}}).explain(true);
print( 'explain = ' + tojson(explain) );
assert.neq( undefined, explain.indexBounds.loc, "C1" );
assert.gt( explain.indexBounds.loc.length, 0, "C2" );

// Check covering.
var covering = explain.indexBounds.loc[0];
for (var i = 1; i < explain.indexBounds.loc.length; ++i) {
    var currentBox = explain.indexBounds.loc[i];
    // min X
    covering[0][0] = Math.min(covering[0][0], currentBox[0][0]);
    // min Y
    covering[0][1] = Math.min(covering[0][1], currentBox[0][1]);
    // max X
    covering[1][0] = Math.max(covering[1][0], currentBox[1][0]);
    // max Y
    covering[1][1] = Math.max(covering[1][1], currentBox[1][1]);
}
print('covering computed from index bounds = ' +
      '(' + covering[0][0] +  ',' + covering[0][1] + ') -->> ' +
      '(' + covering[1][0] +  ',' + covering[1][1] + ')');
// Compare covering against $box coordinates.
// min X
assert.lte(covering[0][0], 4);
// min Y
assert.lte(covering[0][1], 4);
// max X
assert.gte(covering[1][0], 6);
// max Y
assert.gte(covering[1][1], 6);
