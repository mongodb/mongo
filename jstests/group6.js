t = db.jstests_group6;
t.drop();

for( i = 1; i <= 10; ++i ) {
    t.save( {i:new NumberLong( i ),y:1} );
}

assert.eq.automsg( "55", "t.group( {key:'y', reduce:function(doc,out){ out.i += doc.i; }, initial:{i:0} } )[ 0 ].i" );

t.drop();
for( i = 1; i <= 10; ++i ) {
    if ( i % 2 == 0 ) {
        t.save( {i:new NumberLong( i ),y:1} );
    } else {
        t.save( {i:i,y:1} );
    }
}

assert.eq.automsg( "55", "t.group( {key:'y', reduce:function(doc,out){ out.i += doc.i; }, initial:{i:0} } )[ 0 ].i" );

t.drop();
for( i = 1; i <= 10; ++i ) {
    if ( i % 2 == 1 ) {
        t.save( {i:new NumberLong( i ),y:1} );
    } else {
        t.save( {i:i,y:1} );
    }
}

assert.eq.automsg( "55", "t.group( {key:'y', reduce:function(doc,out){ out.i += doc.i; }, initial:{i:0} } )[ 0 ].i" );

