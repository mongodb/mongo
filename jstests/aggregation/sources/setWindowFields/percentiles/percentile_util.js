export function runSetWindowStage(coll, percentileSpec, medianSpec, letSpec) {
    return coll
        .aggregate(
            [
                {$addFields: {str: "hiya"}},
                {
                    $setWindowFields: {
                        sortBy: {_id: 1},
                        output: {
                            runningPercentile: percentileSpec,
                            runningMedian: medianSpec,
                        }
                    }
                }
            ],
            {let : letSpec})
        .toArray();
}

export function assertResultEqToVal({resultArray: results, percentile: pVal, median: mVal}) {
    for (let index = 0; index < results.length; index++) {
        assert.eq(pVal, results[index].runningPercentile);
        assert.eq(mVal, results[index].runningMedian);
    }
}

export function assertResultCloseToVal({resultArray: results, percentile: pVal, median: mVal}) {
    for (let index = 0; index < results.length; index++) {
        // TODO SERVER-91956: Under some circumstances, mongod returns slightly wrong answers due to
        // precision. When mongod has better precision, this function can be removed in favor of
        // assertResultEqToVal.
        for (let percentileIndex = 0; percentileIndex < pVal.length; percentileIndex++) {
            assert.close(pVal[percentileIndex], results[index].runningPercentile[percentileIndex]);
        }
        assert.close(mVal, results[index].runningMedian);
    }
}

export function testError(coll, percentileSpec, expectedCode, letSpec) {
    assert.throwsWithCode(() => coll.aggregate([{
                                                   $setWindowFields: {
                                                       partitionBy: "$ticket",
                                                       sortBy: {ts: 1},
                                                       output: {outputField: percentileSpec},
                                                   }
                                               }],
                                               {let : letSpec}),
                          expectedCode);
}
