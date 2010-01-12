t = db.many;

t.drop();
db.bar.drop();

t.save({x:9});
t.save({x:19});

x = 2;
while( x < 60 ) { 
    patt={};
    patt[x] = 1;
    if( x == 20 )
	patt = { x : 1 };
    t.ensureIndex(patt);
    x++;
}

// print( tojson(db.getLastErrorObj()) );
assert( db.getLastError(), "should have an error 'too many indexes'" );

// 40 is the limit currently

// print( t.getIndexes().length == 40, "40" );

assert( t.getIndexes().length == 40, "40" );

assert( t.find({x:9}).length() == 1, "b" ) ;

t.renameCollection( "bar" );

assert( t.find({x:9}).length() == 0, "c" ) ;

assert( db.bar.find({x:9}).length() == 1, "d" ) ;
