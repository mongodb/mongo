/**
 * Test that a merizos-only aggregation pipeline is explainable, and that the resulting explain plan
 * confirms that the pipeline ran entirely on merizoS.
 */
(function() {
    "use strict";

    const st = new ShardingTest({name: "merizos_comment_test", merizos: 1, shards: 1});
    const merizosConn = st.s;

    const stageSpec = {
        "$listLocalSessions": {allUsers: false, users: [{user: "nobody", db: "nothing"}]}
    };

    // Use the test stage to create a pipeline that runs exclusively on merizoS.
    const merizosOnlyPipeline = [stageSpec, {$match: {dummyField: 1}}];

    // We expect the explain output to reflect the stage's spec.
    const expectedExplainStages = [stageSpec, {$match: {dummyField: {$eq: 1}}}];

    // Test that the merizoS-only pipeline is explainable.
    const explainPlan = assert.commandWorked(merizosConn.getDB("admin").runCommand(
        {aggregate: 1, pipeline: merizosOnlyPipeline, explain: true}));

    // We expect the stages to appear under the 'merizos' heading, for 'splitPipeline' to be
    // null, and for the 'merizos.host' field to be the hostname:port of the merizoS itself.
    assert.docEq(explainPlan.merizos.stages, expectedExplainStages);
    assert.eq(explainPlan.merizos.host, merizosConn.name);
    assert.isnull(explainPlan.splitPipeline);

    st.stop();
})();
