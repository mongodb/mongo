p = function( o ) {
    print( tojson( o ) );
}

f = db.jstests_autoid;
f.drop();

f.save( {z:1} );
a = f.findOne( {z:1} );
p( a );
f.update( {z:1}, {z:2} );
b = f.findOne( {z:2} );
p( b );
//assert.eq( a._id, b._id );
c = f.update( {z:2}, {z:"abcdefgabcdefgabcdefg"} );
c = f.findOne( {} );
p( c );
//assert.eq( a._id, c._id );
