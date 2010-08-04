t = db.jstests_indexi;

t.drop();

for( var a = 0; a < 10; ++a ) {
    for( var b = 0; b < 10; ++b ) {
        for( var c = 0; c < 10; ++c ) {
            t.save( {a:a,b:b,c:c} );
        }
    }
}

t.ensureIndex( {a:1,b:1,c:1} );
t.ensureIndex( {a:1,c:1} );

assert.automsg( "!t.find( {a:{$gt:1,$lt:10},c:{$gt:1,$lt:10}} ).explain().indexBounds.b" );