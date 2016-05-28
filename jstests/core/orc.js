// test that or duplicates are dropped in certain special cases
t = db.jstests_orc;
t.drop();

// The goal here will be to ensure the full range of valid values is scanned for each or clause, in
// order to ensure that
// duplicates are eliminated properly in the cases below when field range elimination is not
// employed.  The deduplication
// of interest will occur on field a.  The range specifications for fields b and c are such that (in
// the current
// implementation) field range elimination will not occur between the or clauses, meaning that the
// full range of valid values
// will be scanned for each clause and deduplication will be forced.

// NOTE This test uses some tricks to avoid or range elimination, but in future implementations
// these tricks may not apply.
// Perhaps it would be worthwhile to create a mode where range elimination is disabled so it will be
// possible to write a more
// robust test.

t.ensureIndex({a: -1, b: 1, c: 1});

// sanity test
t.save({a: null, b: 4, c: 4});
assert.eq(1, t.count({
    $or: [
        {a: null, b: {$gte: 0, $lte: 5}, c: {$gte: 0, $lte: 5}},
        {a: null, b: {$gte: 3, $lte: 8}, c: {$gte: 3, $lte: 8}}
    ]
}));

// from here on is SERVER-2245
t.remove({});
t.save({b: 4, c: 4});
assert.eq(1, t.count({
    $or: [
        {a: null, b: {$gte: 0, $lte: 5}, c: {$gte: 0, $lte: 5}},
        {a: null, b: {$gte: 3, $lte: 8}, c: {$gte: 3, $lte: 8}}
    ]
}));

// t.remove({});
// t.save( {a:[],b:4,c:4} );
// printjson( t.find(
// {$or:[{a:[],b:{$gte:0,$lte:5},c:{$gte:0,$lte:5}},{a:[],b:{$gte:3,$lte:8},c:{$gte:3,$lte:8}}]}
// ).explain() );
// assert.eq( 1, t.count(
// {$or:[{a:[],b:{$gte:0,$lte:5},c:{$gte:0,$lte:5}},{a:[],b:{$gte:3,$lte:8},c:{$gte:3,$lte:8}}]} )
// );
