/**
 * Assert that we can insert a cluster parameter without the correct cluster parameter time without
 * throwing.
 * Tests that the bug described in SERVER-91835 is resolved.
 */

const conn = MongoRunner.runMongod();

const clusterParameterInsertSucceeds = (doc) => {
    assert.commandWorked(conn.getDB('config').clusterParameters.insert(doc));

    let clusterParams = conn.getDB('config').clusterParameters.find().toArray();
    assert(clusterParams.some((clusterParam) => {
        return JSON.stringify(clusterParam) === JSON.stringify(doc);
    }));
};

clusterParameterInsertSucceeds({'_id': 'testIntClusterParameter'});
clusterParameterInsertSucceeds({'_id': 'testStrClusterParameter', 'clusterParameterTime': 'abcd'});
assert.commandFailedWithCode(conn.getDB('config').clusterParameters.insert({'_id': 12345}),
                             ErrorCodes.OperationFailed);
assert.commandFailedWithCode(conn.getDB('config').clusterParameters.insert({'_id': ''}),
                             ErrorCodes.OperationFailed);

MongoRunner.stopMongod(conn);
