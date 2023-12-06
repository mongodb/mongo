import {assertValueOnPlanPath, runWithFastPathsDisabled} from "jstests/libs/optimizer_utils.js";

const t = db.cqf_type_bracket;
t.drop();

// Generate enough documents for index to be preferable if it exists.
for (let i = 0; i < 100; i++) {
    assert.commandWorked(t.insert({a: i}));
    assert.commandWorked(t.insert({a: i.toString()}));
}

const runTest = ({assertPlan}) => {
    {
        const res = t.explain("executionStats").aggregate([{$match: {a: {$lt: "2"}}}]);
        assert.eq(12, res.executionStats.nReturned);
        if (assertPlan) {
            assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
        }
    }
    {
        const res = t.explain("executionStats").aggregate([{$match: {a: {$gt: "95"}}}]);
        assert.eq(4, res.executionStats.nReturned);
        if (assertPlan) {
            assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
        }
    }
    {
        const res = t.explain("executionStats").aggregate([{$match: {a: {$lt: 2}}}]);
        assert.eq(2, res.executionStats.nReturned);
        if (assertPlan) {
            assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
        }
    }
    {
        const res = t.explain("executionStats").aggregate([{$match: {a: {$gt: 95}}}]);
        assert.eq(4, res.executionStats.nReturned);
        if (assertPlan) {
            assertValueOnPlanPath("PhysicalScan", res, "child.child.nodeType");
        }
    }

    assert.commandWorked(t.createIndex({a: 1}));

    {
        const res = t.explain("executionStats").aggregate([{$match: {a: {$lt: "2"}}}]);
        assert.eq(12, res.executionStats.nReturned);
        if (assertPlan) {
            assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
        }
    }
    {
        const res = t.explain("executionStats").aggregate([{$match: {a: {$gt: "95"}}}]);
        assert.eq(4, res.executionStats.nReturned);
        if (assertPlan) {
            assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
        }
    }
    {
        const res = t.explain("executionStats").aggregate([{$match: {a: {$lt: 2}}}]);
        assert.eq(2, res.executionStats.nReturned);
        if (assertPlan) {
            assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
        }
    }
    {
        const res = t.explain("executionStats").aggregate([{$match: {a: {$gt: 95}}}]);
        assert.eq(4, res.executionStats.nReturned);
        if (assertPlan) {
            assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
        }
    }

    assert.commandWorked(t.dropIndex({a: 1}));
};

// We do not get the expected explain output if a query is optimized using a fast path.
runTest({assertPlan: false});
runWithFastPathsDisabled(() => runTest({assertPlan: true}));
