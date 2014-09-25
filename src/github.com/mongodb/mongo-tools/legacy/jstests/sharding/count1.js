// count1.js

s = new ShardingTest( "count1" , 2 , 1 );
db = s.getDB( "test" );

// Stop balancer since doing manual stuff
// Make sure we totally stop here, otherwise balancing round can intermittently slip by
// Counts during balancing are only approximate (as of 7/28/12).
// If we fix that, we should write a test for it elsewhere
s.stopBalancer();

// ************** Test Set #1 *************
// Basic counts on "bar" collections, not yet sharded

db.bar.save( { n : 1 } )
db.bar.save( { n : 2 } )
db.bar.save( { n : 3 } )

assert.eq( 3 , db.bar.find().count() , "bar 1" );
assert.eq( 1 , db.bar.find( { n : 1 } ).count() , "bar 2" );

//************** Test Set #2 *************
// Basic counts on sharded  "foo" collection.
// 1. Create foo collection, insert 6 docs
// 2. Divide into three chunks
// 3. Test counts before chunk migrations
// 4. Manually move chunks.  Now each shard should have 3 docs.
// 5. i. Test basic counts on foo
//    ii. Test counts with limit
//    iii. Test counts with skip
//    iv. Test counts with skip + limit
//    v. Test counts with skip + limit + sorting
// 6. Insert 10 more docs. Further limit/skip testing with a find query
// 7. test invalid queries/values

// part 1
s.adminCommand( { enablesharding : "test" } )
s.adminCommand( { shardcollection : "test.foo" , key : { name : 1 } } );

primary = s.getServer( "test" ).getDB( "test" );
secondary = s.getOther( primary ).getDB( "test" );

assert.eq( 1 , s.config.chunks.count() , "sanity check A" );

db.foo.save( { _id : 1 , name : "eliot" } )
db.foo.save( { _id : 2 , name : "sara" } )
db.foo.save( { _id : 3 , name : "bob" } )
db.foo.save( { _id : 4 , name : "joe" } )
db.foo.save( { _id : 5 , name : "mark" } )
db.foo.save( { _id : 6 , name : "allan" } )

assert.eq( 6 , db.foo.find().count() , "basic count" );

// part 2
s.adminCommand( { split : "test.foo" , find : { name : "joe" } } ); // [Minkey -> allan) , * [allan -> ..)
s.adminCommand( { split : "test.foo" , find : { name : "joe" } } ); // * [allan -> sara) , [sara -> Maxkey)
s.adminCommand( { split : "test.foo" , find : { name : "joe" } } ); // [alan -> eliot) , [eliot -> sara]

// MINKEY->allan,bob->eliot,joe,mark->sara,MAXKEY

s.printChunks()

// part 3
assert.eq( 6 , db.foo.find().count() , "basic count after split " );
assert.eq( 6 , db.foo.find().sort( { name : 1 } ).count() , "basic count after split sorted " );

// part 4
s.adminCommand( { movechunk : "test.foo" , find : { name : "eliot" } , to : secondary.getMongo().name , _waitForDelete : true } );

assert.eq( 3 , primary.foo.find().toArray().length , "primary count" );
assert.eq( 3 , secondary.foo.find().toArray().length , "secondary count" );
assert.eq( 3 , primary.foo.find().sort( { name : 1 } ).toArray().length , "primary count sorted" );
assert.eq( 3 , secondary.foo.find().sort( { name : 1 } ).toArray().length , "secondary count sorted" );

// part 5
// Some redundant tests, but better safe than sorry. These are fast tests, anyway.

// i.
assert.eq( 6 , db.foo.find().count() , "total count after move" );
assert.eq( 6 , db.foo.find().toArray().length , "total count after move" );
assert.eq( 6 , db.foo.find().sort( { name : 1 } ).toArray().length , "total count() sorted" );
assert.eq( 6 , db.foo.find().sort( { name : 1 } ).count() , "total count with count() after move" );

// ii.
assert.eq( 2 , db.foo.find().limit(2).count(true) );
assert.eq( 2 , db.foo.find().limit(-2).count(true) );
assert.eq( 6 , db.foo.find().limit(100).count(true) );
assert.eq( 6 , db.foo.find().limit(-100).count(true) );
assert.eq( 6 , db.foo.find().limit(0).count(true) );

// iii.
assert.eq( 6 , db.foo.find().skip(0).count(true) );
assert.eq( 5 , db.foo.find().skip(1).count(true) );
assert.eq( 4 , db.foo.find().skip(2).count(true) );
assert.eq( 3 , db.foo.find().skip(3).count(true) );
assert.eq( 2 , db.foo.find().skip(4).count(true) );
assert.eq( 1 , db.foo.find().skip(5).count(true) );
assert.eq( 0 , db.foo.find().skip(6).count(true) );
assert.eq( 0 , db.foo.find().skip(7).count(true) );

// iv.
assert.eq( 2 , db.foo.find().limit(2).skip(1).count(true) );
assert.eq( 2 , db.foo.find().limit(-2).skip(1).count(true) );
assert.eq( 5 , db.foo.find().limit(100).skip(1).count(true) );
assert.eq( 5 , db.foo.find().limit(-100).skip(1).count(true) );
assert.eq( 5 , db.foo.find().limit(0).skip(1).count(true) );

assert.eq( 0 , db.foo.find().limit(2).skip(10).count(true) );
assert.eq( 0 , db.foo.find().limit(-2).skip(10).count(true) );
assert.eq( 0 , db.foo.find().limit(100).skip(10).count(true) );
assert.eq( 0 , db.foo.find().limit(-100).skip(10).count(true) );
assert.eq( 0 , db.foo.find().limit(0).skip(10).count(true) );

assert.eq( 2 , db.foo.find().limit(2).itcount() , "LS1" )
assert.eq( 2 , db.foo.find().skip(2).limit(2).itcount() , "LS2" )
assert.eq( 1 , db.foo.find().skip(5).limit(2).itcount() , "LS3" )
assert.eq( 6 , db.foo.find().limit(2).count() , "LSC1" )
assert.eq( 2 , db.foo.find().limit(2).size() , "LSC2" )
assert.eq( 2 , db.foo.find().skip(2).limit(2).size() , "LSC3" )
assert.eq( 1 , db.foo.find().skip(5).limit(2).size() , "LSC4" )
assert.eq( 4 , db.foo.find().skip(1).limit(4).size() , "LSC5" )
assert.eq( 5 , db.foo.find().skip(1).limit(6).size() , "LSC6" )

// SERVER-3567 older negative limit tests
assert.eq( 2 , db.foo.find().limit(2).itcount() , "N1" );
assert.eq( 2 , db.foo.find().limit(-2).itcount() , "N2" );
assert.eq( 2 , db.foo.find().skip(4).limit(2).itcount() , "N3" );
assert.eq( 2 , db.foo.find().skip(4).limit(-2).itcount() , "N4" );

// v.
function nameString( c ){
    var s = "";
    while ( c.hasNext() ){
        var o = c.next();
        if ( s.length > 0 )
            s += ",";
        s += o.name;
    }
    return s;
}
assert.eq( "allan,bob,eliot,joe,mark,sara" ,  nameString( db.foo.find().sort( { name : 1 } ) ) , "sort 1" );
assert.eq( "sara,mark,joe,eliot,bob,allan" ,  nameString( db.foo.find().sort( { name : -1 } ) ) , "sort 2" );

assert.eq( "allan,bob" , nameString( db.foo.find().sort( { name : 1 } ).limit(2) ) , "LSD1" )
assert.eq( "bob,eliot" , nameString( db.foo.find().sort( { name : 1 } ).skip(1).limit(2) ) , "LSD2" )
assert.eq( "joe,mark" , nameString( db.foo.find().sort( { name : 1 } ).skip(3).limit(2) ) , "LSD3" )

assert.eq( "eliot,sara" , nameString( db.foo.find().sort( { _id : 1 } ).limit(2) ) , "LSE1" )
assert.eq( "sara,bob" , nameString( db.foo.find().sort( { _id : 1 } ).skip(1).limit(2) ) , "LSE2" )
assert.eq( "joe,mark" , nameString( db.foo.find().sort( { _id : 1 } ).skip(3).limit(2) ) , "LSE3" )

// part 6
for ( i=0; i<10; i++ ){
    db.foo.save( { _id : 7 + i , name : "zzz" + i } )
}

assert.eq( 10 , db.foo.find( { name : { $gt : "z" } } ).itcount() , "LSF1" )
assert.eq( 10 , db.foo.find( { name : { $gt : "z" } } ).sort( { _id : 1 } ).itcount() , "LSF2" )
assert.eq( 5 , db.foo.find( { name : { $gt : "z" } } ).sort( { _id : 1 } ).skip(5).itcount() , "LSF3" )
assert.eq( 3 , db.foo.find( { name : { $gt : "z" } } ).sort( { _id : 1 } ).skip(5).limit(3).itcount() , "LSF4" )

// part 7
// Make sure count command returns error for invalid queries
var badCmdResult = db.runCommand({ count: 'foo', query: { $c: { $abc: 3 }}});
assert( ! badCmdResult.ok , "invalid query syntax didn't return error" );
assert( badCmdResult.errmsg.length > 0 , "no error msg for invalid query" );

// Negative skip values should return error
var negSkipResult = db.runCommand({ count: 'foo', skip : -2 });
assert( ! negSkipResult.ok , "negative skip value shouldn't work" );
assert( negSkipResult.errmsg.length > 0 , "no error msg for negative skip" );

// Negative skip values with positive limit should return error
var negSkipLimitResult = db.runCommand({ count: 'foo', skip : -2, limit : 1 });
assert( ! negSkipLimitResult.ok , "negative skip value with limit shouldn't work" );
assert( negSkipLimitResult.errmsg.length > 0 , "no error msg for negative skip" );

s.stop();