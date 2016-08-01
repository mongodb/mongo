/**
 * Tests authorization special cases with views. These are special exceptions that prohibit certain
 * operations on views even if the user has an explicit privilege on that view.
 */
(function() {
    "use strict";

    let mongod = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});

    // Create the admin user.
    let adminDB = mongod.getDB("admin");
    assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
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
            {resource: {db: viewsDBName, collection: "permitted"}, actions: ["find"]}
        ],
        roles: []
    }));
    assert.commandWorked(
        viewsDB.runCommand({createUser: "viewUser", pwd: "pwd", roles: ["readWriteView"]}));

    adminDB.logout();
    assert.eq(1, viewsDB.auth("viewUser", "pwd"));

    const lookupStage = {$lookup: {from: "forbidden", localField: "x", foreignField: "x", as: "y"}};
    const graphLookupStage = {
        $graphLookup: {
            from: "forbidden",
            startWith: [],
            connectFromField: "x",
            connectToField: "x",
            as: "y"
        }
    };

    // You cannot create a view if you have both the 'createCollection' and 'find' actions on that
    // view but not the 'find' action on all of the dependent namespaces.
    assert.commandFailedWithCode(viewsDB.createView("view", "forbidden", []),
                                 ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(viewsDB.createView("view", "permitted", [lookupStage]),
                                 ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(viewsDB.createView("view", "permitted", [graphLookupStage]),
                                 ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(
        viewsDB.createView("view", "permitted", [{$facet: {a: [lookupStage]}}]),
        ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(
        viewsDB.createView("view", "permitted", [{$facet: {b: [graphLookupStage]}}]),
        ErrorCodes.Unauthorized);

    assert.commandWorked(viewsDB.createView("view", "permitted", [{$match: {x: 1}}]));

    // You cannot modify a view if you have both the 'collMod' and 'find' actions on that view but
    // not the 'find' action on all of the dependent namespaces.
    assert.commandFailedWithCode(
        viewsDB.runCommand({collMod: "view", viewOn: "forbidden", pipeline: [{$match: {}}]}),
        ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(
        viewsDB.runCommand({collMod: "view", viewOn: "permitted", pipeline: [lookupStage]}),
        ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(
        viewsDB.runCommand({collMod: "view", viewOn: "permitted", pipeline: [graphLookupStage]}),
        ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(
        viewsDB.runCommand(
            {collMod: "view", viewOn: "permitted", pipeline: [{$facet: {a: [lookupStage]}}]}),
        ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(
        viewsDB.runCommand(
            {collMod: "view", viewOn: "permitted", pipeline: [{$facet: {b: [graphLookupStage]}}]}),
        ErrorCodes.Unauthorized);
}());
