//
//testing $sort aggregation pipeline for heterogeneity (SERVER-6125)
//method:   
// Create an array with all the different types. (Array is created with correct sort order)
// Randomise it (to prevent $sort returning types in same order).
// Save the array members to the db.  
// aggregate($sort) 
// iterate through the array ensuring the _ids are in the correct order
 
//to make results array nested (problem 2)
function nestArray( nstArray ) {
    for( x = 0; x < nstArray.length; x++ ) {
        nstArray[x].a = { b :  nstArray[x].a };
    }
}
 
//sort and run the tests
function runSort( chkDoc, nest, problem ){
    var chkArray = setupArray();
    if( nest ){ nestArray( chkArray ); }
    Array.shuffle(chkArray);
    var t = db.s6125;
    t.drop();
    t.insert( chkArray );
     
    runAsserts( t.aggregate( { $sort : chkDoc } ).result, problem );
}
 
//actually run the tests
function runAsserts( chkArray, problem ) {
    //check the _id at [0] to determine which way around this has been sorted
    //then check for gt / lt.  Done rather than neq to preclude a < b > c issues
    if( chkArray[ 0 ]._id == 0 ) {
        for( var x=0; x<chkArray.length-1; x++ ) {
            assert.lt( chkArray[x]._id, chkArray[x + 1]._id );
        }
    }
    else if( chkArray[ chkArray.length - 1 ]._id == 0 ) {
        for( var x=0; x<chkArray.length-1; x++ ) {
            assert.gt( chkArray[x]._id, chkArray[x + 1]._id );
        }
    }
    else {
        assert.eq( true, chkArray[0]._id == 0 || chkArray[chkArray.length-1]._id == 0 );
    }
}
 
//set up data
function setupArray(){
    return [ 
        { _id : 0, a : MinKey, ty : "MinKey" },
        { _id : 1, a : null, ty : "null" },
        { _id : 2, a : 1, ty : "Number" },
        { _id : 3, a : NumberLong(2), ty : "NumberLong"},
        { _id : 4, a : "3", ty : "String" },
        //Symbol not implemented in JS
        { _id : 5, a : {}, ty : "Object" },
        { _id : 6, a : new DBRef( "test.s6125", ObjectId("0102030405060708090A0B0C") ), ty : "DBRef" },
        { _id : 7, a : [ ], ty : "Empty Array" },
        { _id : 8, a : [ 1 , 2 , "a" , "B" ], ty : "Array" },
        { _id : 9, a : BinData(0, "77+9"), ty : "BinData" },
        { _id : 10, a : new ObjectId("0102030405060708090A0B0C"), ty : "ObjectId" },
        { _id : 11, a : true, ty : "Boolean" },
        { _id : 12, a : new Timestamp( 1/1000 , 1 ), ty : "Timestamp" },
        { _id : 13, a : new Date( 2 ), ty : "Date" },
        { _id : 14, a : /regex/, ty : "RegExp" },
        { _id : 15, a : new DBPointer("test.s6125",new ObjectId("0102030405060708090A0B0C")), ty : "DBPointer" },
        { _id : 16, a : function(){}, ty : "Code" },
        //Code with Scope not implemented in JS
        { _id : 17, a : MaxKey, ty : "MaxKey"}
    ]
}

//***
//Begin testing for SERVER-6125
//***
//problem 1, does aggregate $sort work with all types
runSort( { a : 1 }, false, "p1" );
 
//problem 2, does aggregate $sort work with all types nested
runSort( { "a" : 1 }, true, "p2a" )
runSort( { "a.b" : 1 }, true, "p2b" );
 
//problem 3, check reverse order sort
runSort( { a : -1 }, false, "p3" );
 
//problem 4, reverse order sort with nested array
runSort( { "a" : -1 }, true, "p4a" );
runSort( { "a.b" : -1 }, true, "p4b" );