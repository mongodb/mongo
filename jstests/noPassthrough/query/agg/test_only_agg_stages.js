/**
 * Verify that enableTestCommands is required to run test-only agg stages.
 * @tags: [
 *   disables_test_commands,
 * ]
 */

function testEnableTestCommandsDisabled(pipeline) {
    TestData.enableTestCommands = false;
    const conn = MongoRunner.runMongod();
    const db = conn.getDB('admin');
    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: 1,
            pipeline: pipeline,
            cursor: {},
        }),
        ErrorCodes.QueryFeatureNotAllowed,
    );
    MongoRunner.stopMongod(conn);
}

function testEnableTestCommandsEnabled(pipeline) {
    TestData.enableTestCommands = true;
    const conn = MongoRunner.runMongod();
    const db = conn.getDB('admin');
    assert.commandWorked(db.runCommand({
        aggregate: 1,
        pipeline: pipeline,
        cursor: {},
    }));
    MongoRunner.stopMongod(conn);
}

const pipelines = [
    [{$listMqlEntities: {entityType: "aggregationStages"}}],
    [{$listCachedAndActiveUsers: {}}],
];

for (const pipeline of pipelines) {
    testEnableTestCommandsDisabled(pipeline);
    testEnableTestCommandsEnabled(pipeline);
}
