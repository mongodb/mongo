/**
 * Tests authorization special cases with views. These are special exceptions that prohibit certain
 * operations on views even if the user has an explicit privilege on that view.
 */
(function() {
    "use strict";

    function runTest(conn) {
        // Create the admin user.
        let adminDB = conn.getDB("admin");
        assert.commandWorked(
            adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
        assert.eq(1, adminDB.auth("admin", "admin"));

        const viewsDBName = "views_authz";
        let viewsDB = adminDB.getSiblingDB(viewsDBName);
        viewsDB.dropAllUsers();
        viewsDB.logout();

        // Create a user who can read, create and modify a view 'view' and a read a namespace
        // 'permitted' but does not have access to 'forbidden'.
        assert.commandWorked(viewsDB.runCommand({
            createRole: "readWriteView",
            privileges: [
                {
                  resource: {db: viewsDBName, collection: "view"},
                  actions: ["find", "createCollection", "collMod"]
                },
                {resource: {db: viewsDBName, collection: "view2"}, actions: ["find"]},
                {resource: {db: viewsDBName, collection: "permitted"}, actions: ["find"]}
            ],
            roles: []
        }));
        assert.commandWorked(
            viewsDB.runCommand({createUser: "viewUser", pwd: "pwd", roles: ["readWriteView"]}));

        adminDB.logout();
        assert.eq(1, viewsDB.auth("viewUser", "pwd"));

        const lookupStage = {
            $lookup: {from: "forbidden", localField: "x", foreignField: "x", as: "y"}
        };
        const graphLookupStage = {
            $graphLookup: {
                from: "forbidden",
                startWith: [],
                connectFromField: "x",
                connectToField: "x",
                as: "y"
            }
        };

        // You cannot create a view if you have both the 'createCollection' and 'find' actions on
        // that view but not the 'find' action on all of the dependent namespaces.
        assert.commandFailedWithCode(viewsDB.createView("view", "forbidden", []),
                                     ErrorCodes.Unauthorized,
                                     "created a readable view on an unreadable collection");
        assert.commandFailedWithCode(
            viewsDB.createView("view", "permitted", [lookupStage]),
            ErrorCodes.Unauthorized,
            "created a readable view on an unreadable collection via $lookup");
        assert.commandFailedWithCode(
            viewsDB.createView("view", "permitted", [graphLookupStage]),
            ErrorCodes.Unauthorized,
            "created a readable view on an unreadable collection via $graphLookup");
        assert.commandFailedWithCode(
            viewsDB.createView("view", "permitted", [{$facet: {a: [lookupStage]}}]),
            ErrorCodes.Unauthorized,
            "created a readable view on an unreadable collection via $lookup in a $facet");
        assert.commandFailedWithCode(
            viewsDB.createView("view", "permitted", [{$facet: {b: [graphLookupStage]}}]),
            ErrorCodes.Unauthorized,
            "created a readable view on an unreadable collection via $graphLookup in a $facet");

        assert.commandWorked(viewsDB.createView("view", "permitted", [{$match: {x: 1}}]));

        // You cannot modify a view if you have both the 'collMod' and 'find' actions on that view
        // but not the 'find' action on all of the dependent namespaces.
        assert.commandFailedWithCode(
            viewsDB.runCommand({collMod: "view", viewOn: "forbidden", pipeline: [{$match: {}}]}),
            ErrorCodes.Unauthorized,
            "modified a view to read an unreadable collection");
        assert.commandFailedWithCode(
            viewsDB.runCommand({collMod: "view", viewOn: "permitted", pipeline: [lookupStage]}),
            ErrorCodes.Unauthorized,
            "modified a view to read an unreadable collection via $lookup");
        assert.commandFailedWithCode(
            viewsDB.runCommand(
                {collMod: "view", viewOn: "permitted", pipeline: [graphLookupStage]}),
            ErrorCodes.Unauthorized,
            "modified a view to read an unreadable collection via $graphLookup");
        assert.commandFailedWithCode(
            viewsDB.runCommand(
                {collMod: "view", viewOn: "permitted", pipeline: [{$facet: {a: [lookupStage]}}]}),
            ErrorCodes.Unauthorized,
            "modified a view to read an unreadable collection via $lookup in a $facet");
        assert.commandFailedWithCode(
            viewsDB.runCommand({
                collMod: "view",
                viewOn: "permitted",
                pipeline: [{$facet: {b: [graphLookupStage]}}]
            }),
            ErrorCodes.Unauthorized,
            "modified a view to read an unreadable collection via $graphLookup in a $facet");

        // When auth is enabled, users must specify both "viewOn" and "pipeline" when running
        // collMod on a view; specifying only one or the other is not allowed. Without both the
        // "viewOn" and "pipeline" specified, authorization checks cannot determine if the users
        // have the necessary privileges.
        assert.commandFailedWithCode(viewsDB.runCommand({collMod: "view", pipeline: []}),
                                     ErrorCodes.InvalidOptions,
                                     "modified a view without having to specify 'viewOn'");
        assert.commandFailedWithCode(viewsDB.runCommand({collMod: "view", viewOn: "other"}),
                                     ErrorCodes.InvalidOptions,
                                     "modified a view without having to specify 'pipeline'");

        // Create a view on a forbidden collection and populate it.
        assert.eq(1, adminDB.auth("admin", "admin"));
        assert.commandWorked(viewsDB.createView("view2", "forbidden", []));
        for (let i = 0; i < 10; i++) {
            assert.writeOK(viewsDB.forbidden.insert({x: 1}));
        }
        adminDB.logout();

        // Performing a find on a readable view returns a cursor that allows us to perform a getMore
        // even if the underlying collection is unreadable.
        assert.commandFailedWithCode(viewsDB.runCommand({find: "forbidden"}),
                                     ErrorCodes.Unauthorized,
                                     "successfully performed a find on an unreadable namespace");
        let res = viewsDB.runCommand({find: "view2", batchSize: 1});
        assert.commandWorked(res, "could not perform a find on a readable view");
        assert.eq(res.cursor.ns,
                  "views_authz.view2",
                  "performing find on a view does not return a cursor on the view namespace");
        assert.commandWorked(viewsDB.runCommand({getMore: res.cursor.id, collection: "view2"}),
                             "could not perform getMore on a readable view");
    }

    // Run the test on a standalone.
    let mongod = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
    runTest(mongod);
    MongoRunner.stopMongod(mongod);

    // Run the test on a sharded cluster.
    let cluster = new ShardingTest(
        {shards: 1, mongos: 1, keyFile: "jstests/libs/key1", other: {shardOptions: {auth: ""}}});
    runTest(cluster);
    cluster.stop();
}());
