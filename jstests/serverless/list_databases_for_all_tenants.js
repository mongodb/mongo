/**
 * Tests for the listDatabasesForAllTenants command.
 */
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For arrayEq()

// Given the output from the listDatabasesForAllTenants command, ensures that the total size
// reported is the sum of the individual db sizes.
function verifySizeSum(listDatabasesOut) {
    assert(listDatabasesOut.hasOwnProperty("databases"));
    const dbList = listDatabasesOut.databases;
    let sizeSum = 0;
    for (let i = 0; i < dbList.length; i++) {
        sizeSum += dbList[i].sizeOnDisk;
    }
    assert.eq(sizeSum, listDatabasesOut.totalSize);
}

// Given the output from the listDatabasesForAllTenants command, ensures that only the names and
// tenantIds of the databases are listed
function verifyNameOnly(listDatabasesOut) {
    // Delete extra meta info only returned by shardsvrs.
    delete listDatabasesOut.lastCommittedOpTime;

    for (let field in listDatabasesOut) {
        assert(['totalSize', 'totalSizeMb'].every((f) => f != field), 'unexpected field ' + field);
    }
    listDatabasesOut.databases.forEach((database) => {
        for (let field in database) {
            assert(['name', 'tenantId'].some((f) => f == field),
                   'only expected name or tenantId but got: ' + field);
        }
    });
}

// creates 'num' databases on 'conn', each belonging to a different tenant
function createMultitenantDatabases(conn, tokenConn, num) {
    let tenantIds = [];
    let expectedDatabases = [];

    for (let i = 0; i < num; i++) {
        // Randomly generate a tenantId
        let kTenant = ObjectId();
        tenantIds.push(kTenant.str);

        // Create a user for kTenant and then set the security token on the connection.
        assert.commandWorked(conn.getDB('$external').runCommand({
            createUser: "readWriteUserTenant" + i.toString(),
            '$tenant': kTenant,
            roles: [{role: 'readWriteAnyDatabase', db: 'admin'}]
        }));
        tokenConn._setSecurityToken(_createSecurityToken(
            {user: "readWriteUserTenant" + i.toString(), db: '$external', tenant: kTenant}));

        // Create a collection for the tenant and then insert into it.
        const tokenDB = tokenConn.getDB('auto_gen_db_' + i.toString());
        assert.commandWorked(tokenDB.createCollection('coll' + i.toString()));

        expectedDatabases.push(
            {"name": 'auto_gen_db_' + i.toString(), "tenantId": kTenant, "empty": false});
    }
    return [tenantIds, expectedDatabases];
}

// Given the output from the listDatabasesForAllTenants command, ensures that the database entries
// are correct
function verifyDatabaseEntries(listDatabasesOut, expectedDatabases) {
    const fieldsToSkip = ['sizeOnDisk'];
    assert(
        arrayEq(expectedDatabases, listDatabasesOut.databases, undefined, undefined, fieldsToSkip),
        tojson(listDatabasesOut.databases));
}

// Check that command properly lists all databases created by users authenticated with a security
// token
function runTestCheckMultitenantDatabases(primary, numDBs) {
    const adminDB = primary.getDB("admin");
    const tokenConn = new Mongo(primary.host);

    // Add a root user that is unauthorized to run the command
    assert.commandWorked(adminDB.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));

    // Create numDBs databases, each belonging to a different tenant
    const [tenantIds, expectedDatabases] = createMultitenantDatabases(primary, tokenConn, numDBs);

    // Check that all numDB databases were created of the proper size and include the correct
    // database entries
    let cmdRes = assert.commandWorked(
        adminDB.runCommand({listDatabasesForAllTenants: 1, filter: {name: /auto_gen_db_/}}));
    assert.eq(numDBs, cmdRes.databases.length);
    verifySizeSum(cmdRes);
    verifyDatabaseEntries(cmdRes, expectedDatabases);

    return tenantIds;
}

// Test correctness of filter and nameonly options
function runTestCheckCmdOptions(primary, tenantIds) {
    const adminDB = primary.getDB("admin");

    // Create 4 databases to verify the correctness of filter and nameOnly
    assert.commandWorked(primary.getDB("jstest_list_databases_foo").createCollection("coll0", {
        '$tenant': ObjectId(tenantIds[0])
    }));
    assert.commandWorked(primary.getDB("jstest_list_databases_bar").createCollection("coll0", {
        '$tenant': ObjectId(tenantIds[1])
    }));
    assert.commandWorked(primary.getDB("jstest_list_databases_baz").createCollection("coll0", {
        '$tenant': ObjectId(tenantIds[2])
    }));
    assert.commandWorked(primary.getDB("jstest_list_databases_zap").createCollection("coll0", {
        '$tenant': ObjectId(tenantIds[3])
    }));

    // use to verify that the database entries are correct
    const expectedDatabases2 = [
        {"name": "jstest_list_databases_foo", "tenantId": ObjectId(tenantIds[0]), "empty": false},
        {"name": "jstest_list_databases_bar", "tenantId": ObjectId(tenantIds[1]), "empty": false},
        {"name": "jstest_list_databases_baz", "tenantId": ObjectId(tenantIds[2]), "empty": false},
        {"name": "jstest_list_databases_zap", "tenantId": ObjectId(tenantIds[3]), "empty": false}
    ];

    let cmdRes = assert.commandWorked(adminDB.runCommand(
        {listDatabasesForAllTenants: 1, filter: {name: /jstest_list_databases/}}));
    assert.eq(4, cmdRes.databases.length);
    verifySizeSum(cmdRes);
    verifyDatabaseEntries(cmdRes, expectedDatabases2);

    // Now only list databases starting with a particular prefix.
    cmdRes = assert.commandWorked(adminDB.runCommand(
        {listDatabasesForAllTenants: 1, filter: {name: /^jstest_list_databases_ba/}}));
    assert.eq(2, cmdRes.databases.length);
    verifySizeSum(cmdRes);

    // Now return the system admin database and tenants' admin databases.
    cmdRes = assert.commandWorked(
        adminDB.runCommand({listDatabasesForAllTenants: 1, filter: {name: "admin"}}));
    assert.eq(1 + tenantIds.length, cmdRes.databases.length, tojson(cmdRes.databases));
    verifySizeSum(cmdRes);

    // Now return only one tenant admin database.
    cmdRes = assert.commandWorked(adminDB.runCommand({
        listDatabasesForAllTenants: 1,
        filter: {name: "admin", tenantId: ObjectId(tenantIds[2])}
    }));
    assert.eq(1, cmdRes.databases.length, tojson(cmdRes.databases));
    verifySizeSum(cmdRes);

    // Now return only the names.
    cmdRes = assert.commandWorked(adminDB.runCommand({
        listDatabasesForAllTenants: 1,
        filter: {name: /^jstest_list_databases_/},
        nameOnly: true
    }));
    assert.eq(4, cmdRes.databases.length, tojson(cmdRes));
    verifyNameOnly(cmdRes);

    // Now return only the name of the zap database.
    cmdRes = assert.commandWorked(
        adminDB.runCommand({listDatabasesForAllTenants: 1, nameOnly: true, filter: {name: /zap/}}));
    assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
    verifyNameOnly(cmdRes);

    // $expr in filter.
    cmdRes = assert.commandWorked(adminDB.runCommand({
        listDatabasesForAllTenants: 1,
        filter: {$expr: {$eq: ["$name", "jstest_list_databases_zap"]}}
    }));
    assert.eq(1, cmdRes.databases.length, tojson(cmdRes));
    assert.eq("jstest_list_databases_zap", cmdRes.databases[0].name, tojson(cmdRes));
}

// Test that invalid commands fail
function runTestInvalidCommands(primary) {
    const adminDB = primary.getDB("admin");
    const tokenConn = new Mongo(primary.host);

    // $expr with an unbound variable in filter.
    assert.commandFailed(adminDB.runCommand(
        {listDatabasesForAllTenants: 1, filter: {$expr: {$eq: ["$name", "$$unbound"]}}}));

    // $expr with a filter that throws at runtime.
    assert.commandFailed(
        adminDB.runCommand({listDatabasesForAllTenants: 1, filter: {$expr: {$abs: "$name"}}}));

    // No extensions are allowed in filters.
    assert.commandFailed(
        adminDB.runCommand({listDatabasesForAllTenants: 1, filter: {$text: {$search: "str"}}}));
    assert.commandFailed(adminDB.runCommand({
        listDatabasesForAllTenants: 1,
        filter: {
            $where: function() {
                return true;
            }
        }
    }));
    assert.commandFailed(adminDB.runCommand({
        listDatabasesForAllTenants: 1,
        filter: {a: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}}
    }));

    // Remove internal user
    adminDB.dropUser("internalUsr");

    // Create and authenticate as an admin user with root role
    assert(adminDB.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
    assert(adminDB.auth("admin", "pwd"));

    // Check that user is not authorized to call the command
    let cmdRes = assert.commandFailedWithCode(
        adminDB.runCommand({listDatabasesForAllTenants: 1, filter: {name: /auto_gen_db_/}}),
        ErrorCodes.Unauthorized);

    // Add user authenticated with security token and check that they cannot run the command
    const kTenant = ObjectId();
    assert.commandWorked(primary.getDB('$external').runCommand({
        createUser: "unauthorizedUsr",
        '$tenant': kTenant,
        roles: [{role: 'readWriteAnyDatabase', db: 'admin'}]
    }));
    tokenConn._setSecurityToken(
        _createSecurityToken({user: "unauthorizedUsr", db: '$external', tenant: kTenant}));
    const tokenAdminDB = tokenConn.getDB("admin");
    cmdRes = assert.commandFailedWithCode(
        tokenAdminDB.runCommand({listDatabasesForAllTenants: 1, filter: {name: /auto_gen_db_/}}),
        ErrorCodes.Unauthorized);
}

function runTestsWithMultiTenancySupport(featureFlagRequireTenantID) {
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {
            auth: '',
            setParameter: {
                multitenancySupport: true,
                featureFlagMongoStore: true,
                featureFlagRequireTenantID: featureFlagRequireTenantID
            }
        }
    });
    rst.startSet({keyFile: 'jstests/libs/key1'});
    rst.initiate();

    const primary = rst.getPrimary();
    const adminDB = primary.getDB('admin');

    // Create internal system user that is authorized to run the command
    assert.commandWorked(
        adminDB.runCommand({createUser: 'internalUsr', pwd: 'pwd', roles: ['__system']}));
    assert(adminDB.auth("internalUsr", "pwd"));

    const numDBs = 5;
    const tenantIds = runTestCheckMultitenantDatabases(primary, numDBs);
    runTestCheckCmdOptions(primary, tenantIds);
    runTestInvalidCommands(primary);

    rst.stopSet();
}

function runTestNoMultiTenancySupport() {
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {
            auth: '',
            setParameter: {
                multitenancySupport: false,
                featureFlagMongoStore: true,
                featureFlagRequireTenantID: false
            }
        }
    });
    rst.startSet({keyFile: 'jstests/libs/key1'});
    rst.initiate();

    const primary = rst.getPrimary();
    const adminDB = primary.getDB('admin');

    assert.commandWorked(
        adminDB.runCommand({createUser: 'internalUsr', pwd: 'pwd', roles: ['__system']}));
    assert(adminDB.auth("internalUsr", "pwd"));

    assert.commandFailedWithCode(adminDB.runCommand({listDatabasesForAllTenants: 1}),
                                 ErrorCodes.CommandNotSupported);

    rst.stopSet();
}

runTestsWithMultiTenancySupport(true);
runTestsWithMultiTenancySupport(false);
runTestNoMultiTenancySupport();
}());
