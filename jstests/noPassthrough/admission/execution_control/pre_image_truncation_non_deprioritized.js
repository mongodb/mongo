/**
 * Tests that pre-image truncation is executed as a non-deprioritizable operation by verifying
 * that the totalMarkedNonDeprioritizable counter in serverStatus increases during pre-image
 * truncation.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getTotalMarkedNonDeprioritizableCount} from "jstests/noPassthrough/admission/execution_control/libs/execution_control_helper.js";

const dbName = jsTestName();

describe("pre-image truncation is non-deprioritizable", function () {
    let replTest, primary, primaryDb, coll;

    before(function () {
        replTest = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                setParameter: {
                    // Run the pre-image removal job frequently.
                    expiredChangeStreamPreImageRemovalJobSleepSecs: 1,
                    // Ensure truncation markers are created even for small pre-image volumes.
                    preImagesCollectionTruncateMarkersMinBytes: 1,
                },
            },
        });
        replTest.startSet();
        replTest.initiate();
        primary = replTest.getPrimary();
        primaryDb = primary.getDB(dbName);
        coll = primaryDb.getCollection("coll");

        assert.commandWorked(
            primaryDb.createCollection("coll", {changeStreamPreAndPostImages: {enabled: true}}),
        );

        // Insert pre-images during setup so they are older than 1 second by the time the test
        // body runs (replica set startup takes several seconds).
        assert.commandWorked(coll.insert({_id: 1, x: 1}, {writeConcern: {w: "majority"}}));
        for (let i = 0; i < 20; i++) {
            assert.commandWorked(
                coll.update({_id: 1}, {$inc: {x: 1}}, {writeConcern: {w: "majority"}}),
            );
        }
    });

    after(function () {
        replTest.stopSet();
    });

    it("increments totalMarkedNonDeprioritizable during pre-image truncation", function () {
        const nonDeprioBefore = getTotalMarkedNonDeprioritizableCount(primary);

        // Expire pre-images by wall-clock time. Pre-images inserted in before() are already
        // older than 1 second, so they expire immediately when this parameter is applied.
        assert.commandWorked(
            primary.getDB("admin").runCommand({
                setClusterParameter: {
                    changeStreamOptions: {preAndPostImages: {expireAfterSeconds: 1}},
                },
            }),
        );

        // Wait for the periodic job to truncate the expired pre-images.
        const preImagesNs = primary.getDB("config").getCollection("system.preimages");
        assert.soon(
            () => preImagesNs.find().itcount() === 0,
            "pre-images should be truncated by the periodic removal job",
        );

        // The periodic job continues running after truncation, so the counter must increase.
        assert.soon(
            () => getTotalMarkedNonDeprioritizableCount(primary) > nonDeprioBefore,
            "totalMarkedNonDeprioritizable should increase during pre-image truncation",
        );
    });
});
