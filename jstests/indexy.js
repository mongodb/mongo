// Null index keys may be generated for documents that do not match null.  In particular, for index
// { 'a.c':1 } the document { a:[ {}, { c:10 } ] } generates index keys { '':null } and { '':10 }.
// While the query { a:{ $elemMatch:{ c:null } } } has been designated to match this document, the
// query { 'a.c':null } has been designated to not match this document.
//
// Because the above and similar queries do not match the document but do match documents lacking
// any 'a.c' field (which also generate a { '':null } key), a Matcher is required when it is
// necessary to select matching documents for these queries.  As a result, the fast count in which
// the matcher is bypassed (SERVER-1752) cannot be used for such queries.  The following tests check
// that the count is calculated correctly in cases where fast count mode should not be applied.
//
// SERVER-4529

t = db.jstests_indexy;
t.drop();

function assertNoMatch( query ) {
    // Check that no results are returned by a find.
    assert.eq( 0, t.find( query ).itcount() );
    // Check that no results are counted by a count.
    assert.eq( 0, t.find( query ).count() );
}

t.save({a:[{},{c:10}]});
assertNoMatch( { 'a.c':null } );

t.ensureIndex({'a.c':1});
// No equality match against null.
assertNoMatch( { 'a.c':null } );
// No $in match against null.
assertNoMatch( { 'a.c':{ $in:[ null ] } } );
assertNoMatch( { 'a.c':{ $in:[ null, 1 ] } } );
// No match for a query generating the field range [[ minkey, {} ]], which includes the value null,
// on the field 'a.c'.
assertNoMatch( { 'a.c':{ $lt:{} } } );
