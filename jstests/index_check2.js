
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
q2 = { tags : { $all : [ "tag6" , "tag12" ] } }

assert.eq( 120 , t.find( q1 ).itcount() );
assert.eq( 60 , t.find( q2 ).itcount() );

t.ensureIndex( { tags : 1 } );

assert.eq( 120 , t.find( q1 ).itcount() );
assert.eq( 60 , t.find( q2 ).itcount() );

assert.eq( "BtreeCursor tags_1" , t.find( q1 ).explain().cursor );
assert.eq( "BtreeCursor tags_1" , t.find( q2 ).explain().cursor );

scanned1 = t.find(q1).explain().nscanned;
scanned2 = t.find(q2).explain().nscanned;

//assert( scanned2 <= scanned1 , "$all makes query optimizer not work well" );

