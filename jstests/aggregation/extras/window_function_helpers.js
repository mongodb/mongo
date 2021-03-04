/**
 * Create a collection of tickers and prices.
 */
function seedWithTickerData(coll, docsPerTicker) {
    for (let i = 0; i < docsPerTicker; i++) {
        assert.commandWorked(
            coll.insert({_id: i, partIndex: i, ticker: "T1", price: (500 - i * 10)}));

        assert.commandWorked(coll.insert(
            {_id: i + docsPerTicker, partIndex: i, ticker: "T2", price: (400 + i * 10)}));
    }
}

function forEachPartitionCase(callback) {
    callback(null);
    callback("$ticker");
}

function forEachDocumentBounds(callback) {
    callback(["unbounded", 0]);
    callback(["unbounded", -1]);
    callback(["unbounded", 1]);
    callback(["unbounded", 5]);
    callback(["unbounded", "unbounded"]);
}

/**
 * Runs a pipeline containing a $group that computes the window-function equivalent using the
 * given partition key, accumulator, lower, and upper bounds.
 *
 * The bounds are an inclusive range [lower, upper], specified as indexes into the partition (not
 * offsets relative to the current document). For instance, if we're trying to calculate the group
 * equivalent for the 5th document in the partition and bounds [-1, 1], then the caller should set
 * lower to 4 and upper to 6.
 *
 * Note that this function assumes that the data in 'coll' has been seeded with the documents from
 * the seedWithTickerData() method above.
 */
function computeAsGroup({coll, partitionKey, accum, lower, upper}) {
    if (lower < 0 || upper < 0)
        return null;

    let prefixPipe = [{$match: partitionKey}, {$sort: {_id: 1}}, {$skip: lower}];

    // Only attach a $limit if there's a numeric upper bound (or "current"), since "unbounded"
    // implies an infinite limit.
    if (upper != "unbounded")
        prefixPipe = prefixPipe.concat([{$limit: upper + 1}]);

    return coll.aggregate(prefixPipe.concat([{$group: {_id: null, res: {[accum]: "$price"}}}]))
        .toArray()[0]
        .res;
}

/**
 * Runs the given 'accum' as both a window function and its equivalent accumulator in $group across
 * various combinations of partitioning and window bounds. Asserts that the results are consistent.
 *
 * Note that this function assumes that the documents in 'coll' were initialized using the
 * seedWithTickerData() method above.
 */
function testAccumAgainstGroup(coll, accum) {
    forEachPartitionCase(function(partition) {
        forEachDocumentBounds(function(bounds) {
            jsTestLog("Testing accumulator " + accum + " against " + partition +
                      " partition and [" + bounds + "] bounds");
            assert.eq("unbounded", bounds[0], "Only 'unbounded' preceding is supported");
            const wfResults =
                coll.aggregate([
                        {
                            $setWindowFields: {
                                partitionBy: partition,
                                sortBy: {_id: 1},
                                output: {res: {[accum]: "$price", window: {documents: bounds}}}
                            },
                        },
                    ])
                    .toArray();
            for (let index = 0; index < wfResults.length; index++) {
                const wfRes = wfResults[index];

                let groupRes;
                // If there's no partitioning, then only the upper bound increases per doc.
                if (partition == null) {
                    const upperBound = bounds[1] == "unbounded" ? "unbounded" : index + bounds[1];
                    groupRes = computeAsGroup(
                        {coll: coll, partitionKey: {}, accum: accum, lower: 0, upper: upperBound});
                } else {
                    // There is partitioning, so we need to adjust the bounds within the current
                    // partition. Use the 'partIndex' field within each document that gives its
                    // relative offset.
                    const upperBound =
                        bounds[1] == "unbounded" ? "unbounded" : wfRes.partIndex + bounds[1];
                    groupRes = computeAsGroup({
                        coll: coll,
                        partitionKey: {ticker: wfRes.ticker},
                        accum: accum,
                        lower: 0,
                        upper: upperBound
                    });
                }

                // On DEBUG builds, the computed $group may be slightly different due to precision
                // loss when spilling to disk.
                // TODO SERVER-42616: Enable the exact check for $stdDevPop/Samp.
                if (accum == "$stdDevSamp" || accum == "$stdDevPop")
                    assert.close(groupRes,
                                 wfResults[index].res,
                                 "Window function result for index " + index + ": " + tojson(wfRes),
                                 10 /* 10 decimal places */);
                else
                    assert.eq(groupRes,
                              wfResults[index].res,
                              "Window function result for index " + index + ": " + tojson(wfRes));
            }
        });
    });
}
