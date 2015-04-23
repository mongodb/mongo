// Query planner tests for partial indexes.
// Will be converted to unit tests. See SERVER-18092.
(function() {
    "use strict";
    var coll = db.index_partial_queryplanner;
    coll.drop();

    coll.ensureIndex({x: 1},
        {partialFilterExpression: {a: {$lt: 5},
                     b: {$lt: 5}}});

    for(var i = 0; i < 10; i++) {
        coll.insert({x: i, a: i, b: i});
    }

    function useIndex(filter) {
        var query = coll.find(filter);
        var ex = query.explain(true);

        return ex.executionStats.totalDocsExamined <= 1 ||
            ex.executionStats.totalDocsExamined == query.count();
    }

    assert(!useIndex({x: 7, a: 7}));
    assert(!useIndex({x: 7, b: 7}));
    assert(!useIndex({x: 7, a: 7, b: 7}));
    assert(!useIndex({x: 7, a: 7, b: 7, c: 7}));

    assert(!useIndex({x: 3, a: 3}));
    assert(!useIndex({x: 3, b: 3}));
    assert(useIndex({x: 3, a: 3, b: 3}));
    assert(useIndex({x: 3, a: 3, b: 3, c: 3}));
})();
