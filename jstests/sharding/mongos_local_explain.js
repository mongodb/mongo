/**
 * Test that a mongos-only aggregation pipeline is explainable, and that the resulting explain plan
 * confirms that the pipeline ran entirely on mongoS.
 */
(function() {
    "use strict";

    const st = new ShardingTest({name: "mongos_comment_test", mongos: 1, shards: 1});
    const mongosConn = st.s;

    const stageSpec = {
        "$listLocalSessions": {allUsers: false, users: [{user: "nobody", db: "nothing"}]}
    };

    // Use the test stage to create a pipeline that runs exclusively on mongoS.
    const mongosOnlyPipeline = [stageSpec, {$match: {dummyField: 1}}];

    // We expect the explain output to reflect the stage's spec.
    const expectedExplainStages = [stageSpec, {$match: {dummyField: {$eq: 1}}}];

    // Test that the mongoS-only pipeline is explainable.
    const explainPlan = assert.commandWorked(mongosConn.getDB("admin").runCommand(
        {aggregate: 1, pipeline: mongosOnlyPipeline, explain: true}));

    // We expect the stages to appear under the 'mongos' heading, for 'splitPipeline' to be
    // null, and for the 'mongos.host' field to be the hostname:port of the mongoS itself.
    assert.docEq(explainPlan.mongos.stages, expectedExplainStages);
    assert.eq(explainPlan.mongos.host.toLowerCase(), mongosConn.name.toLowerCase());
    assert.isnull(explainPlan.splitPipeline);

    st.stop();
})();
