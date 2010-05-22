/* test using lots of indexes on one collection */

t = db.many;
t.drop();
db.many2.drop();

t.save({x:9});
t.save({x:19});

x = 2;
while( x < 70 ) { 
    patt={};
    patt[x] = 1;
    if( x == 20 )
	patt = { x : 1 };
    t.ensureIndex(patt);
    x++;
}

// print( tojson(db.getLastErrorObj()) );
assert( db.getLastError(), "should have got an error 'too many indexes'" );

// 40 is the limit currently

assert( t.getIndexes().length == 40, "40" );

assert( t.find({x:9}).length() == 1, "b" );
assert(t.find({ x: 9 }).explain().cursor.match(/Btree/), "not using index?");

/* check that renamecollection remaps all the indexes right */
t.renameCollection( "many2" );
assert( t.find({x:9}).length() == 0, "many2a" ) ;
assert( db.many2.find({x:9}).length() == 1, "many2b" ) ;
