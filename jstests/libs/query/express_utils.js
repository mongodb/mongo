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
    expectedFetchCount,
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

    if (expectedFetchCount) {
        const actualFetchCount = explain.executionStats.totalDocsExamined;
        assert.eq(expectedFetchCount,
                  actualFetchCount,
                  "Expected the query to fetch " + expectedFetchCount +
                      " documents, but it fetched " + actualFetchCount + ": " + tojson(explain));
    }
}
