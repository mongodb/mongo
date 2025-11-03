/**
 * Tests that extract_field_paths is not built over user-defined variables.
 *
 * @tags: [
 *    assumes_against_mongod_not_mongos,
 *    # Explain command does not support read concerns other than local.
 *    assumes_read_concern_local,
 *    assumes_read_concern_unchanged,
 *    assumes_unsharded_collection,
 *    requires_fcv_83,
 * ]
 */
import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {getSbePlanStages} from "jstests/libs/query/sbe_explain_helpers.js";

let coll = db.c;
coll.drop();
assert.commandWorked(coll.insert({zero: 0, one: 1, two: 2, nested: {three: 3}}));
const findRes = coll.find({}, {foo: {$let: {vars: {udv: "$nested"}, "in": "$$udv.three"}}}).toArray();
assert.eq(findRes[0].foo, 3);
const expression = {$let: {vars: {CURRENT: "$$ROOT.nested"}, in: {$multiply: ["$three", "$$ROOT.two"]}}};
const answer = 6;
const res = coll.aggregate([{$project: {output: expression}}]).toArray();
assert.eq(res.length, 1, tojson(res));
assert.eq(res[0].output, answer, tojson(res));
const projExplain = coll.explain("executionStats").aggregate([{$project: {output: expression}}]);
if (getEngine(projExplain) === "sbe") {
    const extractStages = getSbePlanStages(projExplain, "extract_field_paths");
    assert.eq(extractStages.length, 0, "Should not have extract_field_paths stage");
}
const result = coll.aggregate({$group: {_id: 0, res: {$sum: expression}}}).toArray();
assert.eq(result, [{_id: 0, res: answer}]);
const groupExplain = coll.explain("executionStats").aggregate({$group: {_id: 0, res: {$sum: expression}}});
if (getEngine(groupExplain) === "sbe") {
    const extractStages = getSbePlanStages(groupExplain, "extract_field_paths");
    assert.eq(extractStages.length, 0, "Should not have extract_field_paths stage");
}
