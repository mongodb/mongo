/* This test ensures that the usePowerOf2 user flag
 * effectively reuses space. The test repeatedly inserts and
 * then deletes a batch of variable-length strings, then checks
 * that doing so does not cause the storageSize to grow. */

// A bunch of strings of length 0 to 100
var var_length_strings =
    [ "" ,
      "aaaaa" ,
      "aaaaaaaaaa" ,
      "aaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ,
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" ]

//insert all the strings
var batch_insert = function(coll){
    for ( i=0; i < var_length_strings.length; i++ ){
        coll.insert( { a : var_length_strings[i] } );
    }
}

//delete the same strings
var batch_delete = function(coll){
    for ( i=0; i < var_length_strings.length; i++ ){
        coll.remove( { a : var_length_strings[i] } );
    }
}

//number of times to repeat batch inserts/deletes
var numrepeats = 1000;

var testStorageSize = function(ns){
    //insert docs and measure storage size
    batch_insert(ns);
    var oldSize = ns.stats().storageSize;

    //remove and add same docs a bunch of times
    for ( n=0 ; n < numrepeats ; n++ ){
        batch_delete(ns);
        batch_insert(ns);
    }

    //check size didn't change
    var newSize = ns.stats().storageSize;
    assert.eq( oldSize , newSize , "storage size changed");
}

/****************** TEST 1 *****************************/

//create fresh collection, set flag to true, test storage size
var coll = "usepower1"
var t = db.getCollection(coll);
t.drop();
db.createCollection(coll);
var res = db.runCommand( { "collMod" : coll ,  "usePowerOf2Sizes" : true } );
assert.eq( res.ok , 1 , "collMod failed" );

res = db.runCommand( { "collMod" : coll , "usePowerOf2Sizess" : true } )
assert.eq( res.ok , 0 , "collMod should have failed: " + tojson( res ) )

testStorageSize(t);

/**************** Test 2 *****************************/

//repeat previous test, but with flag set at creation time
var coll = "usepower2"
var t = db.getCollection(coll);
t.drop();
db.runCommand({"create" : coll,  "flags" : 1 });

testStorageSize(t);
