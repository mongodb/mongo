/**
 * Verifies that the analyze command splits persistent samples exceeding 16MB across multiple contiguous
 * "pages" in the persistent samples collection.
 */

import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import * as PersistentSamplesUtils from "jstests/libs/query/persistent_samples_utils.js";

describe("analyze sample paging", function () {
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

    beforeEach(function () {
        PersistentSamplesUtils.dropSamplesColl(db);
        db[jsTestName()].drop();
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // Inserts `numDocs` documents totaling `totalSizeBytes`, samples all of them, and asserts the resulting sample is split into exactly `expectedNumPages` pages.
    function runPagingBoundaryTest({numDocs, totalSizeBytes, expectedNumPages}) {
        const collName = jsTestName();
        coll = db[collName];
        coll.drop();

        const docs = PersistentSamplesUtils.makeDocsOfTotalSize(numDocs, totalSizeBytes);
        assert.commandWorked(coll.insertMany(docs));

        // sampleSize == numRecords so all docs will be present in the sample.
        assert.commandWorked(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                sampleSize: docs.length,
                samplingMethod: "random",
            }),
        );

        PersistentSamplesUtils.validatePersistentSample(db, {
            sampledCollName: collName,
            samplingMethod: "random",
            requestedSampleSize: docs.length,
            actualSampleSize: docs.length,
            expectedFields: ["pad"],
            expectedNumPages: expectedNumPages,
        });
    }

    it("keeps a small enough sample on a single page", function () {
        runPagingBoundaryTest({
            numDocs: 1000,
            totalSizeBytes: 15 * 1024 * 1024, // 15MB sample
            expectedNumPages: 1,
        });
    });

    it("splits a sample larger than one page across two pages", function () {
        runPagingBoundaryTest({
            numDocs: 1000,
            totalSizeBytes: 16 * 1024 * 1024, // 16MB sample
            expectedNumPages: 2,
        });
    });

    it("splits a very large sample of many small docs into multiple contiguous pages", function () {
        runPagingBoundaryTest({
            numDocs: 30000,
            totalSizeBytes: 33 * 1024 * 1024, // 33MB sample > 2 pages
            expectedNumPages: 3,
        });
    });

    // A doc of the max user BSON size can never be persisted on a page, since a page also
    // carries overhead from metadata and the array wrapping the docs.
    const bigDoc = PersistentSamplesUtils.makeDocOfSize(16 * 1024 * 1024); // 16 MB
    const smallDoc = PersistentSamplesUtils.makeDocOfSize(1024);

    // Inserts `docs` into `collName` in order, giving each one its index as its _id, and runs
    // `analyze` over all of them.
    function analyzeDocs(collName, docs) {
        coll = db[collName];

        // Reassign _id values in index order to ensure they're all different.
        docs.forEach((doc, i) => {
            assert.commandWorked(coll.insert(Object.assign({}, doc, {_id: i})));
        });

        return db.runCommand({
            analyze: collName,
            mode: "sample",
            sampleSize: docs.length,
            samplingMethod: "random",
        });
    }

    // Returns the _ids of the docs in `docs` that are small enough to be persisted.
    function persistableIds(docs) {
        return docs.map((_, i) => i).filter((i) => docs[i] !== bigDoc);
    }

    it("discards documents too large to persist and keeps the rest of the sample", function () {
        const collName = jsTestName();
        // 1 of 20 docs (5%) is unpersistable, which is within the 10% discard budget.
        const docs = [
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            bigDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
            smallDoc,
        ];
        assert.commandWorked(analyzeDocs(collName, docs));

        const pages = PersistentSamplesUtils.validatePersistentSample(db, {
            sampledCollName: collName,
            samplingMethod: "random",
            requestedSampleSize: 20,
            // The oversized doc is dropped, so only 19 docs are persisted.
            actualSampleSize: 19,
            expectedFields: ["pad"],
            // Discarding mid-sample must not split the sample over multiple pages when all of the other docs can fit on a single page.
            expectedNumPages: 1,
        });

        // The discarded doc is the only one that is absent.
        const persistedIds = pages
            .flatMap((page) => page[PersistentSamplesUtils.sampleDocFieldNames.docsField])
            .map((doc) => doc._id);
        assert.sameMembers(
            persistedIds,
            persistableIds(docs),
            "expected every doc except the oversized one to be persisted",
        );

        coll.drop();
    });

    it("fails when the discarded documents exceed 10% of the sample", function () {
        const collName = jsTestName();
        // 2 of 10 docs (20%) are unpersistable, which exceeds the 10% discard budget.
        assert.commandFailedWithCode(
            analyzeDocs(collName, [
                smallDoc,
                smallDoc,
                smallDoc,
                bigDoc,
                smallDoc,
                smallDoc,
                bigDoc,
                smallDoc,
                smallDoc,
                smallDoc,
            ]),
            13106000,
            "analyze should fail when too much of the sample cannot be persisted",
        );

        // Check that nothing was persisted for the failed analyze.
        const samplesColl = PersistentSamplesUtils.getSamplesColl(db);
        const uuid = PersistentSamplesUtils.getCollUUID(db, collName);
        const filter = PersistentSamplesUtils.getSampleLookupFilter(uuid, "random", 10);
        assert.eq(
            0,
            samplesColl.find(filter).itcount(),
            "failed analyze must not persist any page",
        );

        coll.drop();
    });

    it("fails when the only sampled document is too large to persist", function () {
        const collName = jsTestName();
        // If it's the only doc, it's 100% of the sample.
        assert.commandFailedWithCode(
            analyzeDocs(collName, [bigDoc]),
            13106000,
            "analyze should fail when the single sampled document is too large to persist on a page",
        );

        const samplesColl = PersistentSamplesUtils.getSamplesColl(db);
        const uuid = PersistentSamplesUtils.getCollUUID(db, collName);
        const filter = PersistentSamplesUtils.getSampleLookupFilter(uuid, "random", 1);
        assert.eq(
            0,
            samplesColl.find(filter).itcount(),
            "failed analyze must not persist any page",
        );

        coll.drop();
    });
});
