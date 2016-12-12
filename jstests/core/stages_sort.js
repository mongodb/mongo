// Test query stage sorting.
if (false) {
    t = db.stages_sort;
    t.drop();

    var N = 50;
    for (var i = 0; i < N; ++i) {
        t.insert({foo: i, bar: N - i});
    }

    t.ensureIndex({foo: 1});

    // Foo <= 20, descending.
    ixscan1 = {
        ixscan: {
            args: {
                name: "stages_sort",
                keyPattern: {foo: 1},
                startKey: {"": 20},
                endKey: {},
                startKeyInclusive: true,
                endKeyInclusive: true,
                direction: -1
            }
        }
    };

    // Sort with foo ascending.
    sort1 = {sort: {args: {node: ixscan1, pattern: {foo: 1}}}};
    res = db.runCommand({stageDebug: sort1});
    assert.eq(res.ok, 1);
    assert.eq(res.results.length, 21);
    assert.eq(res.results[0].foo, 0);
    assert.eq(res.results[20].foo, 20);

    // Sort with a limit.
    // sort2 = {sort: {args: {node: ixscan1, pattern: {foo: 1}, limit: 2}}};
    // res = db.runCommand({stageDebug: sort2});
    // assert.eq(res.ok, 1);
    // assert.eq(res.results.length, 2);
    // assert.eq(res.results[0].foo, 0);
    // assert.eq(res.results[1].foo, 1);
}
