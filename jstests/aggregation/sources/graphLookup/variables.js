/**
 * Tests to verify that $graphLookup can use the variables defined in an outer scope.
 */
(function() {
    "use strict";

    let local = db.graph_lookup_var_local;
    let foreign = db.graph_lookup_var_foreign;
    local.drop();
    foreign.drop();

    foreign.insert({from: "b", to: "a", _id: 0});
    local.insert({});

    const basicGraphLookup = {
        $graphLookup: {
            from: "graph_lookup_var_foreign",
            startWith: "$$var1",
            connectFromField: "from",
            connectToField: "to",
            as: "resultsFromGraphLookup"
        }
    };

    const lookup = {
        $lookup: {
            from: "graph_lookup_var_local",
            let : {var1: "a"},
            pipeline: [basicGraphLookup],
            as: "resultsFromLookup"
        }
    };

    // Verify that $graphLookup can use the variable 'var1' which is defined in parent $lookup.
    let res = local.aggregate([lookup]).toArray();
    assert.eq(res.length, 1);
    assert.eq(res[0].resultsFromLookup.length, 1);
    assert.eq(res[0].resultsFromLookup[0].resultsFromGraphLookup.length, 1);
    assert.eq(res[0].resultsFromLookup[0].resultsFromGraphLookup[0], {_id: 0, from: "b", to: "a"});

})();
