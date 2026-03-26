/**
 * Test that the change stream pre-images removal job continues even after hitting a spurious
 * "NotWritablePrimary" error.
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getPreImages} from "jstests/libs/query/change_stream_util.js";
import {describe, it, before, after} from "jstests/libs/mochalite.js";

describe("change streams preimages removal", function () {
    let replSetTest = null;

    before(() => {
        // Start replica set with change stream preimages removal job turned off on all nodes.
        replSetTest = new ReplSetTest({
            name: "replSet",
            nodes: 2,
        });
        replSetTest.startSet({
            setParameter: {
                expiredChangeStreamPreImageRemovalJobSleepSecs: 2,
                preImagesCollectionTruncateMarkersMinBytes: 1,
            },
        });
        replSetTest.initiate();
    });

    after(() => {
        replSetTest.stopSet();
    });

    it("checks that pre-images removal job continues after hitting NotWritablePrimary error", () => {
        const primary = replSetTest.getPrimary();

        // Turn off pre-images removal job temporarily.
        const disableFailpoint = configureFailPoint(primary, "disableChangeStreamPreImagesRemover");

        const db = primary.getDB(jsTestName());
        const coll = assertDropAndRecreateCollection(db, jsTestName(), {changeStreamPreAndPostImages: {enabled: true}});

        const n = 10;
        for (let i = 0; i < n; ++i) {
            assert.commandWorked(coll.insertOne({_id: i}));
        }
        for (let i = 0; i < n; ++i) {
            assert.commandWorked(coll.updateOne({_id: i}, {$set: {value: i}}));
        }
        for (let i = 0; i < n; ++i) {
            assert.commandWorked(coll.deleteOne({_id: i}));
        }

        assert.eq(n * 2, getPreImages(primary).length);

        assert.commandWorked(
            primary.adminCommand({
                setClusterParameter: {
                    changeStreamOptions: {preAndPostImages: {expireAfterSeconds: 1}},
                },
            }),
        );

        // Make pre-images removal job fail with "NotWritablePrimary".
        const notWritablePrimaryFailpoint = configureFailPoint(primary, "preImagesRemovalFailsWithNotWritablePrimary");

        // Re-renable pre-images removal job
        disableFailpoint.off();

        // Wait until at least one "NotWritablePrimary" error was hit, and then turn off the fail point.
        // Pre-image removal should resume from now on.
        notWritablePrimaryFailpoint.wait({timesEntered: 1});
        notWritablePrimaryFailpoint.off();

        // Wait until all pre-images were successfully deleted.
        assert.soon(() => {
            return getPreImages(primary).length == 0;
        });
    });
});
