// Basic test for special explain fields of a geo cursor.

t = db.jstests_explain7;
t.drop();

function checkFields( expected, explain ) {
//    printjson( explain );
    for( e in expected ) {
        assert.eq( expected[ e ], explain[ e ], e );
    }
}

t.ensureIndex( { loc:'2d' } );
checkFields( {
            lookedAt:0,
            matchesPerfd:0,
            objectsLoaded:0,
            pointsLoaded:0,
            pointsSavedForYield:0,
            pointsChangedOnYield:0,
            pointsRemovedOnYield:0
            },
            t.find( { loc:[ 1, 5 ] } ).explain( true ) );

t.save( { loc: [ 1, 4 ] } );
t.save( { loc: [ 1, 5 ] } );
checkFields( {
            lookedAt:1,
            matchesPerfd:0,
            objectsLoaded:1,
            pointsLoaded:1,
            pointsSavedForYield:0,
            pointsChangedOnYield:0,
            pointsRemovedOnYield:0
            },
            t.find( { loc:[ 1, 5 ] } ).explain( true ) );

t.save( { loc: [ 1, 5 ] } );
checkFields( {
            n:1,
            lookedAt:2,
            matchesPerfd:0,
            objectsLoaded:2,
            pointsLoaded:2,
            pointsSavedForYield:0,
            pointsChangedOnYield:0,
            pointsRemovedOnYield:0
            },
            t.find( { loc:[ 1, 5 ] } ).limit( 1 ).explain( true ) );
