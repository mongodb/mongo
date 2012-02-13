(function(){

// SERVER-3702

var s = new ShardingTest( "shard8", 2, 0, 2 );
var specialDB = "[a-z]+";
var specialNS = specialDB + ".special";

s.adminCommand( { enablesharding : "test" } );
s.adminCommand( { shardcollection : "test.data" , key : { num : 1 } } );

// Test that the database will not complain "cannot have 2 database names that differs on case"
s.adminCommand( { enablesharding : specialDB } );
s.adminCommand( { shardcollection :  specialNS, key : { num : 1 } } );

var exists = s.getDB("config").collections.find( { _id: specialNS } ).count();
assert.eq( exists, 1 );

// Test that drop database properly cleans up config
s.getDB(specialDB).dropDatabase();

var cursor = s.getDB("config").collections.find( { _id: specialNS } );

if (cursor.hasNext()) {
  assert.eq( cursor.count(), 1 );
  assert( cursor.next()["dropped"] );
}
else {
  // It's alright if not found since it should be deleted
}

})();

