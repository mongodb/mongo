// A reusable set of examples. Each is composed of a set of cursors returned from mongot.

export function searchShardedExampleCursors1(dbName, collNS, collName, expectedCommand) {
    const resultsID = NumberLong(123);
    const metaID = NumberLong(2);
    const historyResults = [
        {
            expectedCommand: expectedCommand,
            response: {
                ok: 1,
                cursors: [
                    {
                        cursor: {
                            id: resultsID,
                            type: "results",
                            ns: collNS,
                            nextBatch: [
                                {_id: 1, val: 1, $searchScore: .4},
                                {_id: 2, val: 2, $searchScore: .3},
                            ],
                        },
                        ok: 1
                    },
                    {
                        cursor: {
                            id: metaID,
                            ns: collNS,
                            type: "meta",
                            nextBatch: [{metaVal: 1}, {metaVal: 2}],
                        },
                        ok: 1
                    }
                ]
            }
        },
        // GetMore for results cursor
        {
            expectedCommand: {getMore: resultsID, collection: collName},
            response: {
                cursor: {
                    id: NumberLong(0),
                    ns: collNS,
                    nextBatch: [{_id: 3, val: 3, $searchScore: 0.123}]
                },
                ok: 1
            }
        },
    ];
    const historyMeta = [
        // GetMore for metadata cursor.
        {
            expectedCommand: {getMore: metaID, collection: collName},
            response: {
                cursor: {id: NumberLong(0), ns: collNS, nextBatch: [{metaVal: 3}, {metaVal: 4}]},
                ok: 1
            }
        },
    ];
    return {
        resultsID: resultsID,
        metaID: metaID,
        historyResults: historyResults,
        historyMeta: historyMeta,
    };
}

export function searchShardedExampleCursors2(dbName, collNS, collName, expectedCommand) {
    const resultsID = NumberLong(3);
    const metaID = NumberLong(4);
    const historyResults = [
        {
            expectedCommand: expectedCommand,
            response: {
                ok: 1,
                cursors: [
                    {
                        cursor: {
                            id: resultsID,
                            type: "results",
                            ns: collNS,
                            nextBatch: [
                                {_id: 5, val: 5, $searchScore: .4},
                                {_id: 6, val: 6, $searchScore: .3},
                            ],

                        },
                        ok: 1,
                    },
                    {
                        cursor: {
                            id: metaID,
                            ns: collNS,
                            type: "meta",
                            nextBatch: [{metaVal: 10}, {metaVal: 11}],
                        },
                        ok: 1
                    }
                ]
            }
        },
        // GetMore for results cursor
        {
            expectedCommand: {getMore: resultsID, collection: collName},
            response: {
                cursor: {
                    id: NumberLong(0),
                    ns: collNS,
                    nextBatch: [{_id: 7, val: 7, $searchScore: 0.123}]
                },
                ok: 1
            }
        },
    ];
    const historyMeta = [
        // GetMore for metadata cursor.
        {
            expectedCommand: {getMore: metaID, collection: collName},
            response: {
                cursor: {id: NumberLong(0), ns: collNS, nextBatch: [{metaVal: 12}, {metaVal: 13}]},
                ok: 1
            }
        },
    ];
    return {
        resultsID: resultsID,
        metaID: metaID,
        historyResults: historyResults,
        historyMeta: historyMeta,
    };
}
