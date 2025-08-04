/**
 * Library with utilies for testing express executor
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {isExpress} from "jstests/libs/query/analyze_plan.js";

export function runExpressTest({
    coll,
    filter,
    project = {},
    limit = 0 /* no limit */,
    collation = {},
    result,
    usesExpress,
    expectedNonZeroFetchCount,
}) {
    const actual = coll.find(filter, project).limit(limit).collation(collation).toArray();
    const explain =
        coll.find(filter, project).limit(limit).collation(collation).explain("executionStats");

    assertArrayEq({
        actual: actual,
        expected: result,
        extraErrorMsg: "Result set comparison failed for find(" + tojson(filter) + ", " +
            tojson(project) + ").limit(" + limit + "). Explain: " + tojson(explain)
    });

    assert.eq(
        usesExpress,
        isExpress(db, explain),
        "Expected the query to " + (usesExpress ? "" : "not ") + "use express: " + tojson(explain));

    if (expectedNonZeroFetchCount) {
        const actualFetchCount = explain.executionStats.totalDocsExamined;
        if (expectedNonZeroFetchCount) {
            assert.gt(actualFetchCount,
                      0,
                      "Expected the query to fetch some documents: " + tojson(explain));
        } else {
            assert.eq(
                0, actualFetchCount, "Expected the query to fetch 0 documents: " + tojson(explain));
        }
    }
}
