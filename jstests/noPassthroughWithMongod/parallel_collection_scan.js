
t = db.parallel_collection_scan;
t.drop();

s = "";
while ( s.length < 10000 )
    s += ".";

for ( i = 0; i < 8000; i++ ) {
    t.insert( { x : i, s : s } );
 }


function iterateSliced() {
    var res = t.runCommand( "parallelCollectionScan", { numCursors : 3 } );
    assert( res.ok, tojson( res ) );
    var count = 0;
    for ( var i = 0; i < res.cursors.length; i++ ) {
        var x = res.cursors[i];
        var cursor = new DBCommandCursor( db.getMongo(), x, 5 );
        count += cursor.itcount();
    }

    return count;
}

assert.eq( iterateSliced(), t.count() );
assert.eq( iterateSliced(), i );
