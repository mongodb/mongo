/**
 * Ensures that a createIndexes command request fails when creating an index with illegal options.
 */

const withOrWithoutAuth = [true, false];

withOrWithoutAuth.forEach((withAuth) => {
    jsTest.log(`Specifying index type in the createIndex command with auth=${withAuth}`);
    const collName = jsTestName();
    const testUser = "mongo";
    const testPass = "mongo";

    let options = {};
    if (withAuth) {
        options = {auth: "", bind_ip: "127.0.0.1"};
    }
    const conn = MongoRunner.runMongod(options);
    const admin = conn.getDB("admin");
    if (withAuth) {
        admin.createUser({user: testUser, pwd: testPass, roles: jsTest.adminUserRoles});
        admin.logout();
        admin.auth({user: testUser, pwd: testPass});
    }
    const test = admin.getSiblingDB("test");

    const legalIndexTypes = [1, "2d", "2dsphere", "text", "hashed"];
    const legalIndexTypesForTimeseries = [1, "2dsphere"];
    const illegalIndexTypes = [
        {type: "queryable_encrypted_range", codes: [ErrorCodes.CannotCreateIndex]},
        {type: "wildcard", codes: [7246202]},
        {type: "columnstore", codes: [ErrorCodes.NotImplemented, ErrorCodes.CannotCreateIndex]},
        {type: "geoHaystack", codes: [ErrorCodes.CannotCreateIndex]},
    ];

    legalIndexTypes.forEach(function(indexType) {
        jsTest.log(`Can create a '${indexType == 1 ? "btree" : indexType}' index auth=${withAuth}`);
        const testCollName = collName + "." + indexType;
        assert.commandWorked(test[testCollName].createIndex({"foo": indexType}));
        test[testCollName].drop();
    });

    legalIndexTypesForTimeseries.forEach(function(indexType) {
        jsTest.log(`Can create a '${
            indexType == 1 ? "btree"
                           : indexType}' index on a timeseries collection auth=${withAuth}`);
        const testCollName = collName + "." + indexType;
        assert.commandWorked(test.runCommand({create: testCollName, timeseries: {timeField: "t"}}));
        assert.commandWorked(test[testCollName].createIndex({"foo": indexType}));
        test[testCollName].drop();
    });

    illegalIndexTypes.forEach((args) => {
        const indexType = args.type;
        const expectedErrorCodes = args.codes;
        jsTest.log(`Cannot create a '${indexType}' index auth=${withAuth}`);
        const testCollName = collName + "." + indexType;
        assert.commandFailedWithCode(
            test[testCollName].createIndex({"foo": indexType}),
            expectedErrorCodes,
        );
        assert(
            !test.getCollectionNames().includes(testCollName),
            `The ${testCollName} collection should not be implicitly created upon failing to create the index.`,
        );
        test[testCollName].drop();
    });

    MongoRunner.stopMongod(conn);
});
