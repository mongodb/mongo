
bigString = "";
while ( bigString.length < 16000 )
    bigString += ".";

t = db.index_bigkeys_update;
t.drop();

t.insert( { _id : 0, x : "asd" } );
t.ensureIndex( { x : 1 } );

assert.eq( 1, t.count() );

t.update( {} , { $set : { x : bigString } } );
err = db.getLastErrorObj();
assert( err.err, err );

assert.eq( 1, t.count() );
assert.eq( "asd", t.findOne().x ); // make sure doc is the old version
assert.eq( "asd", t.findOne( { _id : 0 } ).x ); // make sure doc is the old version
