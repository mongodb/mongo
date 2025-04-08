/**
 * There is a bug in the sbe stage builders where $group issues a getField from a hashed field in a
 * hashed index. This leads to incorrectly returning null. To fix, we disable SBE in this case. This
 * test reproduces several failing cases for passthrough suites.
 *
 * TODO BACKPORT-24552, BACKPORT-24553 - relax this fcv requirement.
 * @tags: [requires_fcv_82, requires_non_retryable_commands]
 */
import {
    TestCases
} from "jstests/libs/query/index_with_hashed_path_prefix_of_nonhashed_path_tests.js";

const coll = db.index_with_hashed_path_prefix_of_nonhashed_path;
for (let testCase of TestCases) {
    coll.drop();
    assert.commandWorked(coll.insertMany(testCase.docs));
    const res = coll.aggregate(testCase.query).toArray();
    assert.eq(testCase.results, res);
    assert.commandWorked(coll.createIndex(testCase.index, {name: testCase.indexName}));
    const resHinted = coll.aggregate(testCase.query, {hint: testCase.indexName}).toArray();
    assert.eq(res, resHinted);
}
