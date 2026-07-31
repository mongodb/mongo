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

    it("fails when a single sampled document is too large to fit on a page", function () {
        const collName = jsTestName();
        coll = db[collName];
        coll.drop();

        const maxSize = 16 * 1024 * 1024; // 16 MB
        const maxSizeDoc = PersistentSamplesUtils.makeDocOfSize(maxSize);
        assert.commandWorked(coll.insert(maxSizeDoc));

        assert.commandFailedWithCode(
            db.runCommand({
                analyze: collName,
                mode: "sample",
                sampleSize: 1,
                samplingMethod: "random",
            }),
            12980001,
            "analyze should fail when a single sampled document is too large to persist on a page",
        );

        // Check that nothing was persisted for the failed analyze.
        const samplesColl = PersistentSamplesUtils.getSamplesColl(db);
        const uuid = PersistentSamplesUtils.getCollUUID(db, collName);
        const filter = PersistentSamplesUtils.getSampleLookupFilter(uuid, "random", 1);
        assert.eq(
            0,
            samplesColl.find(filter).itcount(),
            "failed analyze must not persist any page",
        );
    });
});
