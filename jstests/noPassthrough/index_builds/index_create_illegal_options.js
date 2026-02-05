/**
 * Ensures that a createIndexes command request fails when creating an index with illegal options.
 */

import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

const withOrWithoutAuth = [true, false];

withOrWithoutAuth.forEach((withAuth) => {
    describe(`Specifying index type in the createIndex command with auth=${withAuth}`, function () {
        const collName = jsTestName();
        const testUser = "mongo";
        const testPass = "mongo";

        before(function () {
            let options = {};
            if (withAuth) {
                options = {auth: "", bind_ip: "127.0.0.1"};
            }
            this.conn = MongoRunner.runMongod(options);
            this.admin = this.conn.getDB("admin");
            if (withAuth) {
                this.admin.createUser({user: testUser, pwd: testPass, roles: jsTest.adminUserRoles});
                this.admin.logout();
                this.admin.auth({user: testUser, pwd: testPass});
            }
            this.db = this.admin.getSiblingDB("test");
        });

        const legalIndexTypes = [1, "2d", "2dsphere", "text", "hashed"];
        const legalIndexTypesForTimeseries = [1, "2dsphere"];
        const illegalIndexTypes = [
            {type: "queryable_encrypted_range", codes: [ErrorCodes.CannotCreateIndex]},
            {type: "wildcard", codes: [7246202]},
            {type: "columnstore", codes: [ErrorCodes.NotImplemented]},
            {type: "geoHaystack", codes: [ErrorCodes.CannotCreateIndex]},
        ];

        legalIndexTypes.forEach(function (indexType) {
            it(`Can create a '${indexType == 1 ? "btree" : indexType}' index auth=${withAuth}`, function () {
                this.testCollName = collName + "." + indexType;
                assert.commandWorked(this.db[this.testCollName].createIndex({"foo": indexType}));
            });
        });

        legalIndexTypesForTimeseries.forEach(function (indexType) {
            it(`Can create a '${indexType == 1 ? "btree" : indexType}' index on a timeseries collection auth=${withAuth}`, function () {
                this.testCollName = collName + "." + indexType;
                assert.commandWorked(this.db.runCommand({create: this.testCollName, timeseries: {timeField: "t"}}));
                assert.commandWorked(this.db[this.testCollName].createIndex({"foo": indexType}));
            });
        });

        illegalIndexTypes.forEach((args) => {
            const indexType = args.type;
            const expectedErrorCodes = args.codes;
            it(`Cannot create a '${indexType}' index auth=${withAuth}`, function () {
                this.testCollName = collName + "." + indexType;
                assert.commandFailedWithCode(
                    this.db[this.testCollName].createIndex({"foo": indexType}),
                    expectedErrorCodes,
                );
                assert.doesNotContain(
                    this.db.getCollectionNames(),
                    [this.testCollName],
                    `The ${this.testCollName} collection should not be implicitly created upon failing to create the index.`,
                );
            });
        });

        afterEach(function () {
            this.db[this.testCollName].drop();
        });

        after(function () {
            MongoRunner.stopMongod(this.conn);
        });
    });
});
