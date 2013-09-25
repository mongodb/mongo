// SERVER-3445 Test using coarse multikey bounds for or range elimination.

t = db.jstests_orl;
t.drop();

t.ensureIndex( {'a.b':1,'a.c':1} );
// make the index multikey
t.save( {a:{b:[1,2]}} );

// SERVER-3445
if ( 0 ) {
assert( !t.find( {$or:[{'a.b':2,'a.c':3},{'a.b':2,'a.c':4}]} ).explain().clauses );
}