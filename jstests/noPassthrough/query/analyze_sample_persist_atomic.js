/**
 * Verifies that if the analyze command fails while persisting a sample then the previously-persisted sample is left fully intact.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import * as PersistentSamplesUtils from "jstests/libs/query/persistent_samples_utils.js";

describe("analyze sample persist atomicity under interruption", function () {
    let conn;
    let db;
    let coll;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {featureFlagPersistentStats: true},
        });
        assert.neq(conn, null, "mongod failed to start");
        db = conn.getDB("test");
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("leaves the prior persisted sample unchanged when the replace is interrupted before commit", function () {
        const collName = jsTestName();
        coll = db[collName];
        coll.drop();

        // Insert enough data to create a multi-page sample
        const kNumDocs = 20;
        const kDocSize = 1024 * 1024; // ~1MB
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < kNumDocs; ++i) {
            bulk.insert(PersistentSamplesUtils.makeDocOfSize(kDocSize, i));
        }
        assert.commandWorked(bulk.execute());

        const analyzeCmd = {
            analyze: collName,
            mode: "sample",
            samplingMethod: "random",
            sampleSize: kNumDocs,
        };

        // First analyze succeeds and persists a sample.
        assert.commandWorked(db.runCommand(analyzeCmd));

        const samplesColl = PersistentSamplesUtils.getSamplesColl(db);
        const initialSample = samplesColl.find().sort({"_id.pageNo": 1}).toArray();
        assert.gt(
            initialSample.length,
            1,
            "expected the initial persisted sample to span multiple pages",
            {
                numPages: initialSample.length,
            },
        );
        const initialSampleDocCount = initialSample.reduce((n, page) => n + page.docs.length, 0);

        // Fail the insert during sample replacement so that analyze aborts after it has already staged the delete of the prior sample's pages but before the new pages are inserted.
        const fp = configureFailPoint(conn, "failCollectionInserts", {
            collectionNS: samplesColl.getFullName(),
        });
        assert.commandFailedWithCode(
            db.runCommand(analyzeCmd),
            ErrorCodes.FailPointEnabled,
            "expected analyze to fail with the code injected by failCollectionInserts",
        );

        fp.off();

        // Verify that the original sample is unchanged
        const newSample = samplesColl.find().sort({"_id.pageNo": 1}).toArray();
        assert.eq(
            newSample.length,
            initialSample.length,
            "interrupted analyze changed the number of sample pages",
        );
        const newSampleDocCount = newSample.reduce((n, page) => n + page.docs.length, 0);
        assert.eq(
            newSampleDocCount,
            initialSampleDocCount,
            "interrupted analyze changed the persisted sample",
        );
        assert.sameMembers(
            initialSample.map((p) => p._id.pageNo),
            newSample.map((p) => p._id.pageNo),
        );

        const createdAtField = PersistentSamplesUtils.sampleDocFieldNames.createdAtField;
        assert.sameMembers(
            initialSample.map((p) => p[createdAtField]),
            newSample.map((p) => p[createdAtField]),
            "interrupted analyze changed the persisted sample's createdAt timestamp",
        );

        PersistentSamplesUtils.dropSamplesColl(db);
    });
});
