// Check proper covered index handling when query and processGetMore yield.
// SERVER-4975
// @tags: [
//   uses_parallel_shell,
// ]

if (0) {
    // SERVER-4975

    let t = db.jstests_coveredIndex3;
    let t2 = db.jstests_coveredIndex3_other;
    t.drop();
    t2.drop();

    function doTest(batchSize) {
        // Insert an array, which will make the { a:1 } index multikey and should disable covered
        // index
        // matching.
        let p1 = startParallelShell(
            "for( i = 0; i < 60; ++i ) { \
                               db.jstests_coveredIndex3.save( { a:[ 2000, 2001 ] } ); \
                               sleep( 300 ); \
                           }",
        );

        // Frequent writes cause the find operation to yield.
        let p2 = startParallelShell(
            "for( i = 0; i < 1800; ++i ) { \
                            db.jstests_coveredIndex3_other.save( {} ); \
                            sleep( 10 ); \
                            }",
        );

        for (let i = 0; i < 30; ++i) {
            t.drop();
            t.createIndex({a: 1});

            for (let j = 0; j < 1000; ++j) {
                t.save({a: j});
            }

            let c = t.find({}, {_id: 0, a: 1}).hint({a: 1}).batchSize(batchSize);
            while (c.hasNext()) {
                let o = c.next();
                // If o contains a high numeric 'a' value, it must come from an array saved in p1.
                assert(!(o.a > 1500), "improper object returned " + tojson(o));
            }
        }

        p1();
        p2();
    }

    doTest(2000); // Test query.
    doTest(500); // Try to test getMore - not clear if this will actually trigger the getMore issue.
}
