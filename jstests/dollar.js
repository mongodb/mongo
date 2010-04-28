t = db.jstests_dollar;
t.drop();

function doIt() {
    t.save( { cast: [ "samuel", "john" ], movie: "pulp fiction" } );
    t.save( { cast: [ "john", "hugh", "halle" ], movie: "swordfish" } );
    t.update( { cast: "john" }, { '$set': {'cast.$': "john travolta"} }, false, true );
    assert.eq( 2, t.count( {cast: "john travolta"} ) );
}

doIt();

// TODO fix this
if ( 0 ) {
    t.drop();
    t.ensureIndex( {cast:1} );
    doIt();
}