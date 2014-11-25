// background indexing test during inserts.

assert( db.getName() == "test" );

t = db.bg1;
t.drop();

var a = new Mongo( db.getMongo().host ).getDB( db.getName() );

for( var i = 0; i < 100000; i++ ) {
	t.insert({y:'aaaaaaaaaaaa',i:i});
	if( i % 10000 == 0 ) {
		db.getLastError();
		print(i);
	}
}

//db.getLastError();

// start bg indexing
a.system.indexes.insert({ns:"test.bg1", key:{i:1}, name:"i_1", background:true});

// add more data

for( var i = 0; i < 100000; i++ ) {
	t.insert({i:i});
	if( i % 10000 == 0 ) {
		printjson( db.currentOp() );
		db.getLastError();
		print(i);
	}
}

printjson( db.getLastErrorObj() );

printjson( db.currentOp() );

for( var i = 0; i < 40; i++ ) { 
	if( db.currentOp().inprog.length == 0 )
		break;
	print("waiting");
	sleep(1000);
}

printjson( a.getLastErrorObj() );

var idx = t.getIndexes();
// print("indexes:");
// printjson(idx);

assert( idx[1].key.i == 1 );
