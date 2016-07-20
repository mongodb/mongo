/**
 * Tests that view creation and modification is correctly persisted.
 *
 * This test requires persistence to ensure data survives a restart.
 * @tags: [requires_persistence]
 */
(function() {
    'use strict';

    // The following test verifies that writeConcern: {j: true} ensures that the view catalog is
    // durable.
    let dbpath = MongoRunner.dataPath + '_durable_view_catalog';
    resetDbpath(dbpath);

    let mongodArgs =
        {dbpath: dbpath, noCleanData: true, journal: '', setParameter: "enableViews=1"};

    // Start a mongod.
    let conn = MongoRunner.runMongod(mongodArgs);
    assert.neq(null, conn, 'mongod was unable to start up');

    // Now connect to the mongod, create, remove and modify views and then abruptly stop the server.
    let viewsDB = conn.getDB('test');
    let pipe = [];
    assert.commandWorked(
        viewsDB.runCommand({create: "view1", viewOn: "collection", pipeline: pipe}));
    assert.commandWorked(
        viewsDB.runCommand({create: "view2", viewOn: "collection", pipeline: pipe}));
    assert.commandWorked(
        viewsDB.runCommand({create: "view3", viewOn: "collection", pipeline: pipe}));
    assert.commandWorked(viewsDB.runCommand({collMod: "view3", viewOn: "view2"}));
    // On the final modification, require a sync to ensure durability.
    assert.commandWorked(viewsDB.runCommand({drop: "view1", writeConcern: {j: 1}}));

    // Hard kill the mongod to ensure the data was indeed synced to durable storage.
    MongoRunner.stopMongod(conn, 9);

    // Restart the mongod.
    conn = MongoRunner.runMongod(mongodArgs);
    assert.neq(null, conn, 'mongod was unable to restart after receiving a SIGKILL');

    // Check that our journaled write still is present.
    viewsDB = conn.getDB('test');
    let actualViews = viewsDB.system.views.find().toArray();
    let expectedViews = [
        {
          "_id": "test.view2",
          "viewOn": "collection",
          "pipeline": {

          }
        },
        {
          "_id": "test.view3",
          "viewOn": "view2",
          "pipeline": {

          }
        }
    ];
    assert.eq(actualViews, expectedViews, "view definitions not correctly persisted");
    MongoRunner.stopMongod(conn);
})();
