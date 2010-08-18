t = db.jstests_in4;

function checkRanges( a, b ) {
    assert.eq( a, b );
//    expectedCount = a;
//    r = b;
////    printjson( r );
//    assert.eq.automsg( "expectedCount", "r.a.length" );
//    for( i in r.a ) {
//        assert.eq.automsg( "r.a[ i ][ 0 ]", "r.a[ i ][ 1 ]" );
//    }
//    assert.eq.automsg( "expectedCount", "r.b.length" );
//    for( i in r.b ) {
//        assert.eq.automsg( "r.b[ i ][ 0 ]", "r.b[ i ][ 1 ]" );
//    }
}

t.drop();
t.ensureIndex( {a:1,b:1} );
checkRanges( {a:[[2,2]],b:[[3,3]]}, t.find( {a:2,b:3} ).explain().indexBounds );
checkRanges( {a:[[2,2],[3,3]],b:[[4,4]]}, t.find( {a:{$in:[2,3]},b:4} ).explain().indexBounds );
checkRanges( {a:[[2,2]],b:[[3,3],[4,4]]}, t.find( {a:2,b:{$in:[3,4]}} ).explain().indexBounds );
checkRanges( {a:[[2,2],[3,3]],b:[[4,4],[5,5]]}, t.find( {a:{$in:[2,3]},b:{$in:[4,5]}} ).explain().indexBounds );

checkRanges( {a:[[2,2],[3,3]],b:[[4,10]]}, t.find( {a:{$in:[2,3]},b:{$gt:4,$lt:10}} ).explain().indexBounds );

t.save( {a:1,b:1} );
t.save( {a:2,b:4.5} );
t.save( {a:2,b:4} );
assert.eq.automsg( "2", "t.find( {a:{$in:[2,3]},b:{$in:[4,5]}} ).explain().nscanned" );
assert.eq.automsg( "2", "t.findOne( {a:{$in:[2,3]},b:{$in:[4,5]}} ).a" );
assert.eq.automsg( "4", "t.findOne( {a:{$in:[2,3]},b:{$in:[4,5]}} ).b" );

t.drop();
t.ensureIndex( {a:1,b:1,c:1} );
checkRanges( {a:[[2,2]],b:[[3,3],[4,4]],c:[[5,5]]}, t.find( {a:2,b:{$in:[3,4]},c:5} ).explain().indexBounds );

t.save( {a:2,b:3,c:5} );
t.save( {a:2,b:3,c:4} );
assert.eq.automsg( "1", "t.find( {a:2,b:{$in:[3,4]},c:5} ).explain().nscanned" );
t.remove();
t.save( {a:2,b:4,c:5} );
t.save( {a:2,b:4,c:4} );
assert.eq.automsg( "2", "t.find( {a:2,b:{$in:[3,4]},c:5} ).explain().nscanned" );

t.drop();
t.ensureIndex( {a:1,b:-1} );
ib = t.find( {a:2,b:{$in:[3,4]}} ).explain().indexBounds;
checkRanges( {a:[[2,2]],b:[[4,4],[3,3]]}, ib );
assert.automsg( "ib.b[ 0 ][ 0 ] > ib.b[ 1 ][ 0 ]" );
ib = t.find( {a:2,b:{$in:[3,4]}} ).sort( {a:-1,b:1} ).explain().indexBounds;
checkRanges( {a:[[2,2]],b:[[3,3],[4,4]]}, ib );
assert.automsg( "ib.b[ 0 ][ 0 ] < ib.b[ 1 ][ 0 ]" );
