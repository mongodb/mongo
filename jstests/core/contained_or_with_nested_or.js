// This test was designed to reproduce a memory leak that was fixed by SERVER-35455.
(function() {
    "use strict";

    const coll = db.contained_or_with_nested_or;
    coll.drop();
    assert.commandWorked(coll.insert([
        // Should not match the query:
        {_id: 0, active: false, loc: "USA", agency: "FBI", vip: false},
        {_id: 1, active: false, loc: "RUS", agency: "OTHER", vip: true},
        {_id: 2, active: true, loc: "RUS", agency: "OTHER", vip: false},
        {_id: 3, active: true, loc: "USA", agency: "OTHER", vip: false},
        {_id: 4, active: true, loc: "UK", agency: "OTHER", vip: false},
        {_id: 5, active: true, loc: "UK", agency: "OTHER", vip: true},
        {_id: 6, active: true},
        // Should match the query:
        {_id: 7, active: true, loc: "USA", agency: "FBI", vip: false},
        {_id: 8, active: true, loc: "USA", agency: "CIA", vip: true},
        {_id: 9, active: true, loc: "RUS", agency: "OTHER", vip: true},
        {_id: 10, active: true, loc: "RUS", agency: "KGB"},
    ]));
    assert.commandWorked(coll.createIndexes([{loc: 1}, {agency: 1}, {vip: 1}]));

    // The following query reproduced the memory leak described in SERVER-38601. To catch a
    // regression, we would only expect this test to fail on ASAN variants. Before SERVER-35455 we
    // would construct a plan for one clause of the $or, then realize that the other clause could
    // not be indexed and discard the plan for the first clause in a way that leaks memory.
    const results = coll.find({
                            active: true,
                            $or: [
                                {loc: "USA", $or: [{agency: "FBI"}, {vip: true}]},
                                {loc: "RUS", $or: [{agency: "KGB"}, {vip: true}]}
                            ]
                        })
                        .toArray();

    // Just assert on the matching _ids. We avoid adding a sort to the query above to avoid
    // restricting the plans the query planner can consider.
    const matchingIds = results.map(result => result._id);
    assert.setEq(new Set(matchingIds), new Set([7, 8, 9, 10]));
}());
