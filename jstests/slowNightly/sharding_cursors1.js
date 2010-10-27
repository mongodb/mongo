s = new ShardingTest( "sharding_cursors1" , 2 , 0 , 1 , { chunksize : 1 } )

s.adminCommand( { enablesharding : "test" } );

s.config.settings.find().forEach( printjson )

db = s.getDB( "test" );

bigString = "x"
while (bigString.length < 1024)
    bigString += bigString;
assert.eq(bigString.length, 1024, 'len');

s.adminCommand( { shardcollection : "test.foo" , key : { _id : 1 } } );

toInsert = ( 1 * 1000 * 1000 );
for (var i=0; i < toInsert; i++ ){
    db.foo.insert( { i: i, r: Math.random(), s: bigString } );
    assert.eq(db.getLastError(), null, 'no error'); //SERVER-1541

    if ( i % 1000 == 999 ) {
        print( "already inserted " + ( i + 1 ) );
    }
}

inserted = toInsert;
for (var i=0; i < 10; i++ ){
    //assert.gte(db.foo.count(), toInsert, 'inserted enough'); //sometimes fails
    assert.gte(db.foo.count(), toInsert - 100, 'inserted enough');
    inserted = Math.min(inserted, db.foo.count())
    sleep (100);
}

print("\n\n\n   **** inserted: " + inserted + '\n\n\n');

/* 

var line = 0;
try {
    assert.gte(db.foo.find({}, {_id:1}).itcount(), inserted, 'itcount check - no sort - _id only');
    line = 1;
    assert.gte(db.foo.find({}, {_id:1}).sort({_id:1}).itcount(), inserted, 'itcount check - _id sort - _id only');
    line = 2;

    db.foo.ensureIndex({i:1});
    db.foo.ensureIndex({r:1});
    db.getLastError();
    line = 3;

    assert.gte(db.foo.find({}, {i:1}).sort({i:1}).itcount(), inserted, 'itcount check - i sort - i only');
    line = 4;
    assert.gte(db.foo.find({}, {_id:1}).sort({i:1}).itcount(), inserted, 'itcount check - i sort - _id only');
    line = 5;

    assert.gte(db.foo.find({}, {r:1}).sort({r:1}).itcount(), inserted, 'itcount check - r sort - r only');
    line = 6;
    assert.gte(db.foo.find({}, {_id:1}).sort({r:1}).itcount(), inserted, 'itcount check - r sort - _id only');
    line = 7;

    assert.gte(db.foo.find().itcount(), inserted, 'itcount check - no sort - full');
    line = 8;
    assert.gte(db.foo.find().sort({_id:1}).itcount(), inserted, 'itcount check - _id sort - full');
    line = 9;
    assert.gte(db.foo.find().sort({i:1}).itcount(), inserted, 'itcount check - i sort - full');
    line = 10;
    assert.gte(db.foo.find().sort({r:1}).itcount(), inserted, 'itcount check - r sort - full');
    line = 11;
} catch (e) {
    print("***** finished through line " + line + " before exception");
    throw e;
}

*/

s.stop();
