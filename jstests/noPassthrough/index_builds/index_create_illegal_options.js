/**
 * Ensures that a createIndexes command request fails when creating an index with illegal options.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

describe("Specifying index type in the createIndex command", function() {
    const collName = jsTestName();
    const testUser = "mongo";
    const testPass = "mongo";

    before(function() {
        this.conn = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
        this.admin = this.conn.getDB("admin");
        this.admin.createUser({user: testUser, pwd: testPass, roles: jsTest.adminUserRoles});
        this.admin.logout();
        this.admin.auth({user: testUser, pwd: testPass});
        this.db = this.admin.getSiblingDB("test");
    });

    const illegalIndexTypes = [
        {type: "2dsphere_bucket", codes: [ErrorCodes.IndexOptionsConflict]},
        {type: "queryable_encrypted_range", codes: [ErrorCodes.IndexOptionsConflict]},
        {type: "wildcard", codes: [7246202]},
        {type: "columnstore", codes: [ErrorCodes.NotImplemented]},
        {type: "geoHaystack", codes: [ErrorCodes.CannotCreateIndex]},
    ];

    const legalIndexTypes = [1, "2d", "2dsphere", "text", "hashed"];
    illegalIndexTypes.forEach((args) => {
        const indexType = args.type;
        const expectedErrorCodes = args.codes;
        it(`Cannot create a '${indexType}' index`, function() {
            this.testCollName = collName + "." + indexType;
            assert.commandFailedWithCode(
                this.db[this.testCollName].createIndex({"foo": indexType}),
                expectedErrorCodes,
            );
            // TODO(SERVER-114308): Primary-driven index builds eagerly create the collection, this
            // assertion will fail on those build variants.
            if (!FeatureFlagUtil.isPresentAndEnabled(this.db, "PrimaryDrivenIndexBuilds")) {
                assert.doesNotContain(
                    this.db.getCollectionNames(),
                    [this.testCollName],
                    `The ${this.testCollName} collection should not be implicitly created upon failing to create the index.`,
                );
            }
        });
    });

    legalIndexTypes.forEach(function(indexType) {
        it(`Can create a '${indexType == 1 ? "btree" : indexType}' index`, function() {
            this.testCollName = collName + "." + indexType;
            assert.commandWorked(this.db[this.testCollName].createIndex({"foo": indexType}));
        });
    });

    afterEach(function() {
        this.db[this.testCollName].drop();
    });

    after(function() {
        MongoRunner.stopMongod(this.conn);
    });
});
