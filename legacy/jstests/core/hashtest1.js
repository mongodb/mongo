//hashtest1.js
//Simple tests to check hashing of various types
//make sure that different numeric types hash to same thing, and other sanity checks

var hash = function( v , seed ){
	if (seed)
		return db.runCommand({"_hashBSONElement" : v , "seed" : seed})["out"];
	else
		return db.runCommand({"_hashBSONElement" : v})["out"];
};

var oidHash = hash( ObjectId() );
var oidHash2 = hash( ObjectId() );
var oidHash3 = hash( ObjectId() );
assert(! friendlyEqual( oidHash, oidHash2) , "ObjectIDs should hash to different things");
assert(! friendlyEqual( oidHash, oidHash3) , "ObjectIDs should hash to different things");
assert(! friendlyEqual( oidHash2, oidHash3) , "ObjectIDs should hash to different things");

var intHash = hash( NumberInt(3) );
var doubHash = hash( 3 );
var doubHash2 = hash( 3.0 );
var longHash = hash( NumberLong(3) );
var fracHash = hash( NumberInt(3.5) );
assert.eq( intHash , doubHash );
assert.eq( intHash , doubHash2 );
assert.eq( intHash , longHash );
assert.eq( intHash , fracHash );

var trueHash = hash( true );
var falseHash = hash( false );
assert(! friendlyEqual( trueHash, falseHash) , "true and false should hash to different things");

var nullHash = hash( null );
assert(! friendlyEqual( falseHash , nullHash ) , "false and null should hash to different things");

var dateHash = hash( new Date() );
sleep(1);
var isodateHash = hash( ISODate() );
assert(! friendlyEqual( dateHash, isodateHash) , "different dates should hash to different things");

var stringHash = hash( "3" );
assert(! friendlyEqual( intHash , stringHash ), "3 and \"3\" should hash to different things");

var regExpHash = hash( RegExp("3") );
assert(! friendlyEqual( stringHash , regExpHash) , "\"3\" and RegExp(3) should hash to different things");

var intHash4 = hash( 4 );
assert(! friendlyEqual( intHash , intHash4 ), "3 and 4 should hash to different things");

var intHashSeeded = hash( 4 , 3 );
assert(! friendlyEqual(intHash4 , intHashSeeded ), "different seeds should make different hashes");

var minkeyHash = hash( MinKey );
var maxkeyHash = hash( MaxKey );
assert(! friendlyEqual(minkeyHash , maxkeyHash ), "minkey and maxkey should hash to different things");

var arrayHash = hash( [0,1.0,NumberLong(2)] );
var arrayHash2 = hash( [0,NumberInt(1),2] );
assert.eq( arrayHash , arrayHash2 , "didn't squash numeric types in array");

var objectHash = hash( {"0":0, "1" : NumberInt(1), "2" : 2} );
assert(! friendlyEqual(objectHash , arrayHash2) , "arrays and sub-objects should hash to different things");

var c = hash( {a : {}, b : 1} );
var d = hash( {a : {b : 1}} );
assert(! friendlyEqual( c , d ) , "hashing doesn't group sub-docs and fields correctly");

var e = hash( {a : 3 , b : [NumberLong(3), {c : NumberInt(3)}]} );
var f = hash( {a : NumberLong(3) , b : [NumberInt(3), {c : 3.0}]} );
assert.eq( e , f , "recursive number squashing doesn't work");

var nanHash = hash( 0/0 );
var zeroHash = hash( 0 );
assert.eq( nanHash , zeroHash , "NaN and Zero should hash to the same thing");


//should also test that CodeWScope hashes correctly
//but waiting for SERVER-3391 (CodeWScope support in shell)