/**
 *  Performance tests for various finders
 */

var calls = 100;
var size = 500000;
var collection_name = "sort2";

function testSetup(dbConn) {
    var t = dbConn[collection_name];
    t.drop();

    for (var i=0; i<size; i++){
        t.save({ num : i });
        if (i == 0 )
            t.ensureIndex( { num : 1 } );
    }
}

function resetQueryCache( db ) {
    db[ collection_name ].createIndex( { a: 1 }, "dumbIndex" );
    db[ collection_name ].dropIndex( "dumbIndex" );
}

function between( low, high, val, msg ) {
    assert( low < val, msg );
    assert( val < high, msg );
}

/**
 *  Tests fetching a set of 10 objects in sorted order, comparing getting
 *  from  front of collection vs end, using $lt
 */
function testFindLTFrontBack(dbConn) {

    var results = {};
    var t = dbConn[collection_name];

    resetQueryCache( dbConn );
    results.oneInOrderLTFirst = Date.timeFunc(
        function(){
            assert( t.find( { num : {$lt : 20} } ).sort( { num : 1 } ).limit(10).toArray().length == 10);
        } , calls );

    resetQueryCache( dbConn );
    results.oneInOrderLTLast = Date.timeFunc(
        function(){
            assert( t.find( { num : {$lt : size-20 }} ).sort( { num : 1 } ).limit(10).toArray().length == 10);
        } , calls );


    between( 0.9, 1.1, results.oneInOrderLTFirst / results.oneInOrderLTLast,
        "first / last (" + results.oneInOrderLTFirst + " / " + results.oneInOrderLTLast + " ) = " +
        results.oneInOrderLTFirst /  results.oneInOrderLTLast + " not in [0.9, 1.1]" );
}



/**
 *  Tests fetching a set of 10 objects in sorted order, comparing getting
 *  from  front of collection vs end
 */
function testFindGTFrontBack(dbConn) {

    var results = {};
    var t = dbConn[collection_name];
    
    resetQueryCache( dbConn );
    results.oneInOrderGTFirst = Date.timeFunc(
        function(){
            assert( t.find( { num : {$gt : 5} } ).sort( { num : 1 } ).limit(10).toArray().length == 10);
        } , calls );

    resetQueryCache( dbConn );
    results.oneInOrderGTLast = Date.timeFunc(
        function(){
            assert( t.find( { num : {$gt : size-20 }} ).sort( { num : 1 } ).limit(10).toArray().length == 10);
        } , calls );


    between( 0.25, 4.0, results.oneInOrderGTFirst / results.oneInOrderGTLast,
            "first / last (" + results.oneInOrderGTFirst + " / " + results.oneInOrderGTLast + " ) = " +
            results.oneInOrderGTFirst /  results.oneInOrderGTLast + " not in [0.25, 4.0]" );

}

testSetup(db);

testFindLTFrontBack(db);
testFindGTFrontBack(db);