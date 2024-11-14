/**
 * Tests that MongoDB can detect file level corruption on collections or indexes during validation
 * and doesn't crash when validate {full: true}.
 *
 * @tags: [requires_wiredtiger]
 */

import {
    corruptFile,
    getUriForColl,
    getUriForIndex,
} from "jstests/disk/libs/wt_file_helper.js";

const baseName = jsTestName();
const collName = "test";
const dbpath = MongoRunner.dataPath + baseName + "/";

function runCorruptionTest({corruptCollection = false, corruptIndex = false}) {
    resetDbpath(dbpath);

    let mongod = MongoRunner.runMongod({dbpath: dbpath});
    let db = mongod.getDB("test");
    let testColl = db.getCollection(collName);

    testColl.insert({a: 1});
    if (corruptIndex) {
        testColl.createIndex({a: 1});
    }

    let collUri = corruptCollection ? getUriForColl(testColl) : null;
    let indexUri = corruptIndex ? getUriForIndex(testColl, "a_1") : null;
    MongoRunner.stopMongod(mongod);

    if (corruptCollection) {
        corruptFile(dbpath + collUri + ".wt");
    }
    if (corruptIndex) {
        corruptFile(dbpath + indexUri + ".wt");
    }

    mongod = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
    db = mongod.getDB("test");
    testColl = db.getCollection(collName);
    const result = testColl.validate({full: true});
    assert(result.valid == false, tojson(result));
    if (corruptIndex) {
        assert(!result.indexDetails.a_1.valid);
    }
    // Even if there is damage the command itself should succeed.
    assert(result.ok);
    MongoRunner.stopMongod(mongod, null, {skipValidation: true});
}

runCorruptionTest({corruptCollection: true});
runCorruptionTest({corruptIndex: true});
runCorruptionTest({corruptCollection: true, corruptIndex: true});
