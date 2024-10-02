/**
 * Tests documents deleted by TTL have recorded pre-images exposed by the 'fullDocumentBeforeChange'
 * field in change events.
 *
 * @tags: [requires_fcv_72]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {ttlMonitorSleepSecs: 1},
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB(jsTestName());
const cst = new ChangeStreamTest(testDB);

const collName = "coll_with_pre_images";
const coll = assertDropAndRecreateCollection(
    testDB, collName, {changeStreamPreAndPostImages: {enabled: true}});

const doc0 = {
    _id: 0,
    lastModifiedDate: new Date()
};
const doc1 = {
    _id: 1,
    lastModifiedDate: new Date()
};

coll.insert(doc0);
coll.insert(doc1);

const csCursor = cst.startWatchingChanges(
    {collection: coll, pipeline: [{$changeStream: {"fullDocumentBeforeChange": "whenAvailable"}}]});

coll.createIndex({lastModifiedDate: 1}, {"expireAfterSeconds": 1});

assert.soon(() => {
    return coll.count() == 0;
});

const expectedChanges = [
    {
        documentKey: {_id: doc0._id},
        ns: {db: testDB.getName(), coll: coll.getName()},
        operationType: "delete",
        fullDocumentBeforeChange: doc0
    },
    {
        documentKey: {_id: doc1._id},
        ns: {db: testDB.getName(), coll: coll.getName()},
        operationType: "delete",
        fullDocumentBeforeChange: doc1
    },
];
cst.assertNextChangesEqualUnordered({cursor: csCursor, expectedChanges});

rst.stopSet();
