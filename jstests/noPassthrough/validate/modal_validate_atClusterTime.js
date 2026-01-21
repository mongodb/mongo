/**
 * Tests that modal validation automatically skips using 'atClusterTime' for unreplicated
 * collections when validating every collection.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {parseValidateOutputsFromLogs} from "jstests/noPassthrough/validate/libs/validate_find_repl_set_divergence.js";

const dbpath = MongoRunner.dataPath + jsTestName();

// Insert something to a replicated collection.
const rst = new ReplSetTest({nodes: 1});
rst.startSet({dbpath: dbpath});
rst.initiate();

const dbName = jsTestName();
const collName = jsTestName();
const primary = rst.getPrimary();
const db = primary.getDB(dbName);

let res = assert.commandWorked(db.runCommand({insert: collName, documents: [{a: 1}]}));

rst.stopSet(null /* signal */, true /* forRestart */);

// Run modal validation with 'atClusterTime'.
MongoRunner.runMongod({
    dbpath: dbpath,
    validate: "",
    setParameter: {
        collectionValidateOptions: {options: {atClusterTime: res.operationTime}},
    },
    noCleanData: true,
});
const validateResults = parseValidateOutputsFromLogs();
jsTest.log.info(`Validate result:\n${tojson(validateResults)}`);
validateResults.forEach((result) => {
    assert(result.attr.results.valid);
});
