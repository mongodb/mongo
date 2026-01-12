/**
 * Ensures that a createIndexes command request fails when creating an index with illegal options.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// Specifying index type in the createIndex command.
const collName = jsTestName();
const testUser = "mongo";
const testPass = "mongo";

const conn = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
const admin = conn.getDB("admin");
admin.createUser({user: testUser, pwd: testPass, roles: jsTest.adminUserRoles});
admin.logout();
admin.auth({user: testUser, pwd: testPass});
const test = admin.getSiblingDB("test");

const illegalIndexTypes = [
    {type: "2dsphere_bucket", codes: [ErrorCodes.IndexOptionsConflict]},
    {type: "queryable_encrypted_range", codes: [ErrorCodes.IndexOptionsConflict]},
    {type: "wildcard", codes: [7246202]},
    {type: "columnstore", codes: [ErrorCodes.NotImplemented, ErrorCodes.CannotCreateIndex]},
    {type: "geoHaystack", codes: [ErrorCodes.CannotCreateIndex]},
];

const legalIndexTypes = [1, "2d", "2dsphere", "text", "hashed"];

// Cannot create illegal indexes.
illegalIndexTypes.forEach((args) => {
    const indexType = args.type;
    const expectedErrorCodes = args.codes;
    const testCollName = collName + "." + indexType;
    assert.commandFailedWithCode(
        test[testCollName].createIndex({"foo": indexType}),
        expectedErrorCodes,
    );
    // TODO(SERVER-114308): Primary-driven index builds eagerly create the collection, this assertion will fail on those build variants.
    if (!FeatureFlagUtil.isPresentAndEnabled(test, "PrimaryDrivenIndexBuilds")) {
        assert.doesNotContain(
            test.getCollectionNames(),
            [testCollName],
            `The ${testCollName} collection should not be implicitly created upon failing to create the index.`,
        );
    }
    test[testCollName].drop();
});

// Can create illegal indexes.
legalIndexTypes.forEach(function (indexType) {
        const testCollName = collName + "." + indexType;
        assert.commandWorked(test[testCollName].createIndex({"foo": indexType}));
    test[testCollName].drop();
});

MongoRunner.stopMongod(conn);
