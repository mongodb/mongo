/**
 * Tests the behavior of the view catalog when resolving views in the face of concurrent
 * invalidations.
 *
 * We tag the test to only run on the WiredTiger storage engine as other storage engines may have
 * locking behavior that may cause the direct updates in system.views to hang.
 * @tags: [requires_wiredtiger]
 */
(function() {
    "use strict";

    const name0 = "invalidation_during_resolution_collection";
    const name1 = "invalidation_during_resolution_view1";
    const name2 = "invalidation_during_resolution_view2";

    const ns0 = db.getCollection(name0);
    const ns1 = db.getCollection(name1);
    const ns2 = db.getCollection(name2);
    [ns0, ns1, ns2].forEach(ns => ns.drop());
    db.system.views.drop();

    // Populate the database with a collection and two views.
    assert.commandWorked(ns0.insert({name: "max"}));
    assert.commandWorked(ns0.insert({name: "xiangyu"}));
    assert.commandWorked(ns0.insert({name: "kyle"}));
    assert.commandWorked(ns0.insert({name: "jungsoo"}));
    assert.commandWorked(db.createView(name1, name0, [{$skip: 1}]));
    assert.commandWorked(db.createView(name2, name1, [{$skip: 1}]));

    // Sanity check.
    assert.eq(ns0.find().itcount(), 4);
    assert.eq(ns1.find().itcount(), 3);
    assert.eq(ns2.find().itcount(), 2);

    // Set a failpoint such that the view catalog is hung in the middle of resolving views.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "hangDuringViewResolution", mode: {skip: 1}}));

    // Start a parallel shell that will manipulate the view catalog. The original state of
    //      ns2 -> ns1 -> ns0
    // will become
    //      ns2 -> ns1 -> ns3 -> ns0
    let awaitViewCollMod = startParallelShell(() => {
        const name0 = "invalidation_during_resolution_collection";
        const name1 = "invalidation_during_resolution_view1";
        const name2 = "invalidation_during_resolution_view2";
        const name3 = "invalidation_during_resolution_view3";

        // Wait for the find command below to make progress and hang.
        jsTestLog("Parallel shell waiting for find to hang.");
        sleep(5000);

        try {
            // Modify the view catalog via system.views. (We can't use the collMod command, since it
            // requires an exclusive collection lock that is currently held in intent shared mode by
            // the thread performing the find.)
            jsTestLog("Parallel shell modifying view catalog.");
            assert.commandWorked(
                db.system.views.update({_id: `test.${name1}`}, {$set: {viewOn: name3}}));
            assert.commandWorked(db.system.views.remove({_id: `test.${name3}`}));
            assert.commandWorked(db.system.views.insert(
                {_id: `test.${name3}`, viewOn: name0, pipeline: [{$skip: 1}]}));

            // Force an invalidation of the view catalog, which will cause it to be reloaded again
            // even during the middle of view resolution.
            jsTestLog("Parallel shell invalidating view catalog.");
            assert.commandWorked(db.runCommand({invalidateViewCatalog: 1}));
        } finally {
            // Now that we're done manipulating the view catalog, allow the command performing ns1
            // resolution to complete.
            jsTestLog("Parallel shell disabling fail point.");
            assert.commandWorked(
                db.adminCommand({configureFailPoint: "hangDuringViewResolution", mode: "off"}));
        }
    });

    // Perform a find on the ns2 that requires multiple steps to resolve. We expect this to see the
    // latest changes to the view catalog and see two documents, despite facing an invalidation
    // mid-resolution.
    const kFiveMinutesInMillis = 1000 * 60 * 5;
    assert.soon(() => {
        assert.eq(ns2.find().itcount(), 1);
        return true;
    }, "find on view timed out", kFiveMinutesInMillis);
    awaitViewCollMod();
}());
