// Test geo index selection criterion, with and without explicit hints. SERVER-4531

t = db.jstests_geog;
t.drop();

t.save({a:{x:0,y:0},z:1});

query = {a:[0,0],z:1};

function assertNotTryingGeoIndex() {
    plans = t.find(query).explain(true).allPlans;
    for( i in plans ) {
        assert( !plans[i].cursor.match( /Geo/ ) );
    }
}

function assertNonGeoResults() {
    assert.eq( 0, t.find(query).itcount() );
    assertNotTryingGeoIndex();
}

function assertNonGeoResultsWithHint() {
    assert.eq( 0, t.find(query).hint({a:1}).itcount() );
    assert.eq( 0, t.find(query).hint({$natural:1}).itcount() );
}

function assertGeoResults() {
    assert.eq( 1, t.find(query).itcount() );
}

function assertGeoResultsWithHint() {
    assert.eq( 1, t.find(query).hint({a:'2d',z:1}).itcount() );
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
