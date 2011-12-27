// Test geo index selection criterion, with and without explicit hints. SERVER-4531

t = db.jstests_geog;
t.drop();

t.save({a:{x:0,y:0},z:1});

query = {a:[0,0],z:1};

function assertCount( count, hint ) {
    if ( hint ) {
        assert.eq( count, t.find(query).hint(hint).itcount() );
    }
    else {
        assert.eq( count, t.find(query).itcount() );
        assert.eq( count, t.count(query) );
    }
}

function assertNotTryingGeoIndex() {
    plans = t.find(query).explain(true).allPlans;
    for( i in plans ) {
        assert( !plans[i].cursor.match( /Geo/ ) );
    }
}

function assertNonGeoResults() {
    assertCount( 0 );
    assertNotTryingGeoIndex();
}

function assertNonGeoResultsWithHint() {
    assertCount( 0, {a:1} );
    assertCount( 0, {$natural:1} );
}

function assertGeoResults() {
    assertCount( 1 );
}

function assertGeoResultsWithHint() {
    assertCount( 1, {a:'2d',z:1} );
}

assertNonGeoResults();

t.ensureIndex({a:'2d',z:1});
// If geo is the only index option, pick it.
assertGeoResults();
assertGeoResultsWithHint();

t.ensureIndex({a:1});
// If there is a non geo index, don't use the geo index.
assertNonGeoResults();
assertNonGeoResultsWithHint();
assertGeoResultsWithHint();

t.dropIndex({a:1});
assertGeoResults();
assertGeoResultsWithHint();
