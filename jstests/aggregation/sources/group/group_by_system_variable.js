/*
 * Tests that the server doesn't crash when you group by a system variable.
 * Reproduces SERVER-57164.
 */
(function() {
"use strict";

function testAggWithSystemVariable(varName, explain) {
    try {
        // This query might or might not throw depending on the engine used
        // and whether the variable is defined.
        if (explain)
            call.explain().aggregate({$group: {_id: varName}});
        else
            call.aggregate({$group: {_id: varName}});
    } catch (e) {
    } finally {
        // Make sure the server didn't crash.
        db.hello();
    }
}

testAggWithSystemVariable("$$IS_MR", true);
testAggWithSystemVariable("$$JS_SCOPE", true);
testAggWithSystemVariable("$$CLUSTER_TIME", true);
testAggWithSystemVariable("$$IS_MR");
testAggWithSystemVariable("$$JS_SCOPE");
testAggWithSystemVariable("$$CLUSTER_TIME");
})();
