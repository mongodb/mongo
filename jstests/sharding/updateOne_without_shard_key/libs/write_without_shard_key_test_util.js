/*
 * Utilities for performing writes without shard key under various test configurations.
 */

'use strict';

function setupShardedCollection(st, nss, shardKey, splitPoints, chunksToMove) {
    const splitString = namespace.split(".");
    const dbName = splitString[0];

    assert.commandWorked(st.s.adminCommand({enablesharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {[shardKey]: 1}}));

    for (let splitPoint of splitPoints) {
        assert.commandWorked(st.s.adminCommand({split: nss, middle: splitPoint}));
    }

    for (let {query, shardName} of chunksToMove) {
        assert.commandWorked(st.s.adminCommand({
            moveChunk: nss,
            find: query,
            to: shardName,
        }));
    }
}

function validateResult(docs, expectedMods) {
    expectedMods.forEach(mod => {
        let field = Object.keys(mod)[0];
        let value = mod[field];
        let docsHaveFieldMatchArray = [];
        docs.forEach(doc => {
            if (doc[field] == value) {
                docsHaveFieldMatchArray.push(doc[field] == value);
            }
        });

        assert(docsHaveFieldMatchArray.length == 1);
    });
}

/*
 * Inserts a batch of documents and runs a write without shard key and returns all of the documents
 * inserted.
 */
function insertDocsAndRunCommand(st, dbName, collName, docsToInsert, cmdObj) {
    assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert(docsToInsert));
    assert.commandWorked(st.s.getDB(dbName).runCommand(cmdObj));
    return st.s.getDB(dbName).getCollection(collName).find({}).toArray();
}

/*
 * Runs a test using a cmdObj with multiple configs e.g. {ordered: true/false}.
 */
function runWithMultipleConfigs(params) {
    params.configs.forEach(config => {
        let newCmdObj = Object.assign({}, config, params.cmdObj);
        let allMatchedDocs = insertDocsAndRunCommand(
            params.st, params.dbName, params.collName, params.docsToInsert, newCmdObj);
        validateResult(allMatchedDocs, params.expectedMods);

        // Clean up the collection for the next test case without dropping the collection.
        assert.commandWorked(
            params.st.s.getDB(params.dbName).getCollection(params.collName).remove({}));
    });
}
