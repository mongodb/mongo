// test simple index filters

t = db.index_filtered2;
t.drop();

t.ensureIndex( { x : 1 },
               { partialFilterExpression : { a : { $lt : 5 }, b : { $lt : 5 } } } );

for ( i = 0; i < 10; i++ ) {
    t.insert( { x : i, a : i, b : i } );
}

function getNumKeys() {
    var res = t.validate(true);
    return res.keysPerIndex[t.getFullName() + ".$x_1"];
}

function useIndex(filter) {
    var query = t.find(filter);
    var ex = query.explain(true);
    /*
    print("----");
    printjson(filter);
    printjson(ex.executionStats.totalDocsExamined);
    printjson(query.count());
    */
    return ex.executionStats.totalDocsExamined <= 1 ||
        ex.executionStats.totalDocsExamined == query.count();
}

assert(!useIndex({x : 7, a : 7}));
assert(!useIndex({x : 7, b : 7}));
assert(!useIndex({x : 7, a : 7, b : 7}));
assert(!useIndex({x : 7, a : 7, b : 7, c : 7}));

assert(!useIndex({x : 3, a : 3}));
assert(!useIndex({x : 3, b : 3}));
assert(useIndex({x : 3, a : 3, b : 3}));
assert(useIndex({x : 3, a : 3, b : 3, c : 3}));
