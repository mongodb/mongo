(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    var coll = db.explain_multikey;
    var keyPattern = {
        a: 1,
        "b.c": 1,
        "b.d": 1,
    };

    /**
     * Creates an index with a key pattern of 'keyPattern' on a collection containing a single
     * document and runs the specified command under explain.
     *
     * @param {Object} testOptions
     * @param {Object} testOptions.docToInsert - The document to insert into the collection.
     * @param {Object} testOptions.commandObj - The operation to run "explain" on.
     * @param {string} testOptions.stage - The plan summary name of the winning plan.
     *
     * @returns {Object} The "queryPlanner" information of the stage with the specified plan summary
     * name.
     */
    function createIndexAndRunExplain(testOptions) {
        coll.drop();

        assert.commandWorked(coll.createIndex(keyPattern));
        assert.writeOK(coll.insert(testOptions.docToInsert));

        var explain = db.runCommand({explain: testOptions.commandObj});
        assert.commandWorked(explain);

        assert(planHasStage(explain.queryPlanner.winningPlan, testOptions.stage),
               "expected stage to be present: " + tojson(explain));
        return getPlanStage(explain.queryPlanner.winningPlan, testOptions.stage);
    }

    // Calls createIndexAndRunExplain() twice: once with a document that causes the created index to
    // be multikey, and again with a document that doesn't cause the created index to be multikey.
    function verifyMultikeyInfoInExplainOutput(testOptions) {
        // Insert a document that should cause the index to be multikey.
        testOptions.docToInsert = {
            a: 1,
            b: [{c: ["w", "x"], d: 3}, {c: ["y", "z"], d: 4}],
        };
        var stage = createIndexAndRunExplain(testOptions);

        assert.eq(true, stage.isMultiKey, "expected index to be multikey: " + tojson(stage));
        if (jsTest.options().storageEngine !== "mmapv1") {
            assert.eq(
                {a: [], "b.c": ["b", "b.c"], "b.d": ["b"]}, stage.multiKeyPaths, tojson(stage));
        } else {
            // Path-level multikey tracking is supported for all storage engines that use the
            // KVCatalog. MMAPv1 is the only storage engine that does not.
            //
            // TODO SERVER-22727: Store path-level multikey information in MMAPv1 index catalog.
            assert(!stage.hasOwnProperty("multiKeyPaths"), tojson(stage));
        }

        // Drop the collection and insert a document that shouldn't cause the index to be multikey.
        testOptions.docToInsert = {
            a: 1,
            b: {c: "w", d: 4},
        };
        stage = createIndexAndRunExplain(testOptions);

        assert.eq(false, stage.isMultiKey, "expected index not to be multikey: " + tojson(stage));
        if (jsTest.options().storageEngine !== "mmapv1") {
            assert.eq({a: [], "b.c": [], "b.d": []}, stage.multiKeyPaths, tojson(stage));
        } else {
            // Path-level multikey tracking is supported for all storage engines that use the
            // KVCatalog. MMAPv1 is the only storage engine that does not.
            //
            // TODO SERVER-22727: Store path-level multikey information in MMAPv1 index catalog.
            assert(!stage.hasOwnProperty("multiKeyPaths"), tojson(stage));
        }
    }

    verifyMultikeyInfoInExplainOutput({
        commandObj: {find: coll.getName(), hint: keyPattern},
        stage: "IXSCAN",
    });

    verifyMultikeyInfoInExplainOutput({
        commandObj: {count: coll.getName(), hint: keyPattern},
        stage: "COUNT_SCAN",
    });

    verifyMultikeyInfoInExplainOutput({
        commandObj: {distinct: coll.getName(), key: "a"},
        stage: "DISTINCT_SCAN",
    });
})();
