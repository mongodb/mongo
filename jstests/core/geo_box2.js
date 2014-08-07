
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
