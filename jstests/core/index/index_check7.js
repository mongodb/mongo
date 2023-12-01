// @tags: [
//   assumes_balancer_off,
//   assumes_read_concern_local,
// ]

import {getOptimizer} from "jstests/libs/analyze_plan.js";

const t = db.index_check7;
t.drop();

assert.commandWorked(t.createIndex({x: 1}));

let docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({_id: i, x: i});
}
assert.commandWorked(t.insert(docs));

let explainResult = t.find({x: 27}).explain(true);
switch (getOptimizer(explainResult)) {
    case "classic": {
        assert.eq(1,
                  explainResult.executionStats.totalKeysExamined,
                  'explain x=27 with index {x : 1}: ' + tojson(explainResult));

        assert.commandWorked(t.createIndex({x: -1}));

        explainResult = t.find({x: 27}).explain(true);
        assert.eq(1,
                  explainResult.executionStats.totalKeysExamined,
                  'explain x=27 with indexes {x : 1} and {x: -1}: ' + tojson(explainResult));

        explainResult = t.find({x: {$gt: 59}}).explain(true);
        assert.eq(40,
                  explainResult.executionStats.totalKeysExamined,
                  'explain x>59 with indexes {x : 1} and {x: -1}: ' + tojson(explainResult));
        break;
    }
    case "CQF": {
        // TODO SERVER-77719: Ensure that the decision for using the scan lines up with CQF
        // optimizer. M2: allow only collscans, M4: check bonsai behavior for index scan.
        break;
    }
    default:
        break
}
