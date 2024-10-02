import "jstests/libs/sbe_assert_error_override.js";
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

var coll = db.reverseArray;
coll.drop();

// We need a document to flow through the pipeline, even though we don't care what fields it
// has.
coll.insert({
    nullField: null,
    undefField: undefined,
    embedded: [[1, 2], [3, 4]],
    singleElem: [1],
    normal: [1, 2, 3],
    num: 1,
    empty: []
});

assertErrorCode(coll, [{$project: {reversed: {$reverseArray: 1}}}], 34435);
assertErrorCode(coll, [{$project: {reversed: {$reverseArray: "$num"}}}], 34435);

var res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: [1, 2]}}}}]);
var output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [2, 1]);

res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: [[1, 2]]}}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [[1, 2]]);

res = coll.aggregate([{$project: {reversed: {$reverseArray: "$notAField"}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, null);

res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: [[1, 2], [3, 4]]}}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [[3, 4], [1, 2]]);

res = coll.aggregate([{$project: {reversed: {$reverseArray: "$embedded"}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [[3, 4], [1, 2]]);

res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: null}}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, null);

res = coll.aggregate([{$project: {reversed: {$reverseArray: "$nullField"}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, null);

res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: undefined}}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, null);

res = coll.aggregate([{$project: {reversed: {$reverseArray: "$undefField"}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, null);

res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: [1]}}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [1]);

res = coll.aggregate([{$project: {reversed: {$reverseArray: "$singleElem"}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [1]);

res = coll.aggregate([{$project: {reversed: {$reverseArray: "$normal"}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, [3, 2, 1]);

res = coll.aggregate([{$project: {reversed: {$reverseArray: {$literal: []}}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, []);

res = coll.aggregate([{$project: {reversed: {$reverseArray: "$empty"}}}]);
output = res.toArray();
assert.eq(1, output.length);
assert.eq(output[0].reversed, []);
