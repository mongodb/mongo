/**
 * Test logging of $queryStats.
 * @tags: [requires_fcv_72]
 */

let options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};

function countMatching(arr, func) {
    let count = 0;
    let i = arr.length;
    while (i--) {
        if (func(arr[i])) {
            count += 1;
        }
    }
    return count;
}

function runQueryStatsAndVerifyLogs({pipeline, transformed, logLevel}) {
    const conn = MongoRunner.runMongod(options);
    const db = conn.getDB('test');
    var coll = db[jsTestName()];
    coll.drop();

    // Set the logLevel.
    assert.commandWorked(db.setLogLevel(logLevel, "queryStats"));

    // Insert a document and run two find command with different shapes so that we have two entries
    // in the queryStats store.
    assert.commandWorked(coll.insert({foo: 1, bar: "hello"}));
    assert.eq(coll.find({foo: 1}).itcount(), 1);
    assert.eq(coll.find({bar: "hello"}).itcount(), 1);

    // Run $queryStats with the specified pipeline.
    let result = conn.adminCommand({aggregate: 1, pipeline: [pipeline], cursor: {}});
    assert.commandWorked(result);

    if (logLevel >= 1) {
        // Checking that we log the invocation of $queryStats.
        let spec = transformed
            ? {"transformIdentifiers": {"algorithm": "hmac-sha-256", "hmacKey": "###"}}
            : {};
        assert(checkLog.checkContainsWithCountJson(
                   conn, 7808300, {"commandSpec": spec}, 1, null, true),
               "failed to find log with id " + 7808300);
    }

    // We only log the output of $queryStats when invoked with transformation, and log level at
    // least 3.
    if (logLevel >= 3 && transformed) {
        // Checking that we are logging both entries in the query stats store.
        assert(checkLog.checkContainsWithCountJson(conn, 7808301, {}, 2, null, true),
               "failed to find log with id " + 7808301);

        // Checking that the output is what we expect.
        const query = assert.commandWorked(db.adminCommand({getLog: "global"}));
        assert(query.hasOwnProperty("log"), "no log field");
        assert.eq(countMatching(query.log, function(v) {
                      const has = (s) => v.indexOf(s) !== -1;
                      return has("thisOutput") && has("key:") && has("queryShape") &&
                          (has("?number") || has("?string"));
                  }), 2);

        // Checking that we log when we are done outputting the results of $queryStats.
        assert(checkLog.checkContainsWithCountJson(conn, 7808302, {}, 1, null, true),
               "failed to find log with id " + 7808302);
    }

    // At a log level of 0 we should not be seeing any of these logs.
    if (logLevel == 0) {
        assert(checkLog.checkContainsWithCountJson(conn, 7808300, {}, 0, null, true));
        assert(checkLog.checkContainsWithCountJson(conn, 7808301, {}, 0, null, true));
        assert(checkLog.checkContainsWithCountJson(conn, 7808302, {}, 0, null, true));
    }

    MongoRunner.stopMongod(conn);
}

// Testing logging with transformation.
const hmacKey = "MjM0NTY3ODkxMDExMTIxMzE0MTUxNjE3MTgxOTIwMjE=";
let pipeline = {
    $queryStats: {transformIdentifiers: {algorithm: "hmac-sha-256", hmacKey: BinData(8, hmacKey)}}
};
runQueryStatsAndVerifyLogs({pipeline: pipeline, transformed: true, logLevel: 3});
runQueryStatsAndVerifyLogs({pipeline: pipeline, transformed: true, logLevel: 1});
runQueryStatsAndVerifyLogs({pipeline: pipeline, transformed: true, logLevel: 0});

// Testing logging without transformation.
pipeline = {
    $queryStats: {}
};
runQueryStatsAndVerifyLogs({pipeline: pipeline, transformed: false, logLevel: 3});
runQueryStatsAndVerifyLogs({pipeline: pipeline, transformed: false, logLevel: 1});
runQueryStatsAndVerifyLogs({pipeline: pipeline, transformed: false, logLevel: 0});
