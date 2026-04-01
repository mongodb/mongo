/**
 * Test that the metrics for change streams pre-images sampling are correctly reflected in
 * the serverStatus output.
 *
 * @tags: [
 *   featureFlagUseReplicatedTruncatesForDeletions
 *  ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {describe, it, before, after} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const kCollName = jsTestName();

describe("change streams pre-images metrics in serverStatus output", () => {
    let replSetTest;

    const setupPreImagesCollection = (db, suffix, n) => {
        const coll = assertDropAndRecreateCollection(db, kCollName + suffix);

        assert.commandWorked(db.runCommand({collMod: coll.getName(), changeStreamPreAndPostImages: {enabled: true}}));

        let bulk = coll.initializeOrderedBulkOp();
        for (let i = 0; i < n; ++i) {
            bulk.insert({_id: i, value: "foo"});
        }
        assert.commandWorked(bulk.execute());

        // Create the pre-images.
        bulk = coll.initializeOrderedBulkOp();
        for (let i = 0; i < n; ++i) {
            bulk.find({_id: i}).updateOne({$set: {value: i}});
        }
        assert.commandWorked(bulk.execute());
    };

    const getMarkerCreationMetrics = (conn) => {
        return assert.commandWorked(conn.adminCommand({serverStatus: 1})).changeStreamPreImages.markerCreation;
    };

    before(() => {
        // Start replica set with change stream preimages removal job turned off on all nodes.
        replSetTest = new ReplSetTest({
            name: "replSet",
            // Set priority 1 on both nodes to prevent priority 0 from being auto-assigned to the
            // standby in disagg testing.
            nodes: [{rsConfig: {priority: 1}}, {rsConfig: {priority: 1}}],
        });
        replSetTest.startSet();
        replSetTest.initiate();
    });

    after(() => {
        replSetTest.stopSet();
    });

    it("checks that serverStatus metrics for markerCreation are updated on every step-up", () => {
        const primary = replSetTest.getPrimary();
        const secondary = replSetTest.getSecondary();

        // Insert initial pre-images into one collection.
        setupPreImagesCollection(primary.getDB(jsTestName()), "initial", 10);
        replSetTest.awaitReplication();

        // Get original metrics from secondary and then step it up.
        let oldMetrics = getMarkerCreationMetrics(secondary);
        jsTest.log.info(`Metrics before step-up: ${tojsononeline(oldMetrics)}`);

        replSetTest.stepUp(secondary);

        // Wait for pre-images sampling pass to complete on old secondary.
        let newMetrics;
        assert.soon(() => {
            newMetrics = getMarkerCreationMetrics(secondary);
            jsTest.log.info(`Metrics after step-up: ${tojsononeline(newMetrics)}`);

            // This is needed because server-side metrics are not updated atomically.
            return (
                newMetrics.totalPass > oldMetrics.totalPass &&
                newMetrics.scannedInternalCollections > oldMetrics.scannedInternalCollections
            );
        });

        assert.gt(newMetrics.totalPass, oldMetrics.totalPass);
        assert.gt(newMetrics.scannedInternalCollections, oldMetrics.scannedInternalCollections);

        // Create three more collections with pre-images.
        setupPreImagesCollection(secondary.getDB(jsTestName()), "one", 5);
        setupPreImagesCollection(secondary.getDB(jsTestName()), "two", 10);
        setupPreImagesCollection(secondary.getDB(jsTestName()), "three", 50);
        replSetTest.awaitReplication();

        // Capture metrics on old primary (now a secondarY).
        oldMetrics = getMarkerCreationMetrics(primary);
        replSetTest.stepUp(primary);

        // Wait for pre-images sampling pass to complete on old primary.
        assert.soon(() => {
            newMetrics = getMarkerCreationMetrics(primary);

            // This is needed because server-side metrics are not updated atomically.
            return (
                newMetrics.totalPass > oldMetrics.totalPass &&
                newMetrics.scannedInternalCollections > oldMetrics.scannedInternalCollections + 3
            );
        });

        assert.gt(newMetrics.totalPass, oldMetrics.totalPass);
        assert.gt(newMetrics.scannedInternalCollections, oldMetrics.scannedInternalCollections + 3);
    });
});
