
t = db.index_check2;
t.drop();

for ( var i=0; i<1000; i++ ){
    var a = [];
    for ( var j=1; j<5; j++ ){
        a.push( "tag" + ( i * j % 50 ));
    }
    t.save( { num : i , tags : a } );
}

q1 = { tags : "tag6" };
q2 = { tags : "tag12" };
q3 = { tags : { $all : [ "tag6" , "tag12" ] } }

assert.eq( 120 , t.find( q1 ).itcount() , "q1 a");
assert.eq( 120 , t.find( q2 ).itcount() , "q2 a" );
assert.eq( 60 , t.find( q3 ).itcount() , "q3 a");

t.ensureIndex( { tags : 1 } );

assert.eq( 120 , t.find( q1 ).itcount() , "q1 a");
assert.eq( 120 , t.find( q2 ).itcount() , "q2 a" );
assert.eq( 60 , t.find( q3 ).itcount() , "q3 a");

assert.eq( "BtreeCursor tags_1" , t.find( q1 ).explain().cursor , "e1" );
assert.eq( "BtreeCursor tags_1" , t.find( q2 ).explain().cursor , "e2" );
assert.eq( "BtreeCursor tags_1" , t.find( q3 ).explain().cursor , "e3" );

scanned1 = t.find(q1).explain().nscanned;
scanned2 = t.find(q2).explain().nscanned;
scanned3 = t.find(q3).explain().nscanned;

//print( "scanned1: " + scanned1 + " scanned2: " + scanned2 + " scanned3: " + scanned3 );

// $all should just iterate either of the words
assert( scanned3 <= Math.max( scanned1 , scanned2 ) , "$all makes query optimizer not work well" );

exp3 = t.find( q3 ).explain();
assert.eq( exp3.indexBounds.tags[0][0], exp3.indexBounds.tags[0][1], "$all range not a single key" );
