/**
 * Tests that an extension can serialize itself for query execution correctly.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insert({_id: 1}));

// The $shardedExecutionSerialization stage uasserts with the following code in sharded environments.
const kShardedExecutionSerializationCode = 11173701;
{
    const pipeline = [{$shardedExecutionSerialization: {}}];
    if (FixtureHelpers.isMongos(db)) {
        assert.throwsWithCode(() => coll.aggregate(pipeline), kShardedExecutionSerializationCode);
    } else {
        assert.doesNotThrow(() => coll.aggregate(pipeline));
    }
}
