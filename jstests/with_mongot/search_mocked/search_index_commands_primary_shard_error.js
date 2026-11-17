/**
 * Tests that search index commands and $listSearchIndexes on mongos surface the error returned by
 * the primary shard when it fails the listCollections or _shardsvrResolveView commands that mongos
 * uses to resolve the target namespace.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const dbName = jsTestName();
const collName = "coll";
const viewName = "view";
const errorMsg = "primary shard failed namespace resolution";

function commandsOn(nss) {
    return [
        {
            createSearchIndexes: nss,
            indexes: [{name: "indexName", definition: {mappings: {dynamic: true}}}],
        },
        {updateSearchIndex: nss, name: "indexName", definition: {mappings: {dynamic: true}}},
        {dropSearchIndex: nss, name: "indexName"},
        {listSearchIndexes: nss},
        {aggregate: nss, pipeline: [{$listSearchIndexes: {}}], cursor: {}},
    ];
}

describe("search index commands on mongos with a failing primary shard", function () {
    before(function () {
        this.mongotMock = new MongotMock();
        this.mongotMock.start();
        const mockHost = this.mongotMock.getConnection().host;

        this.st = new ShardingTest({
            mongos: 1,
            shards: 1,
            other: {
                mongosOptions: {setParameter: {searchIndexManagementHostAndPort: mockHost}},
                rsOptions: {setParameter: {searchIndexManagementHostAndPort: mockHost}},
            },
        });
        this.testDB = this.st.s.getDB(dbName);

        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );
        assert.commandWorked(this.testDB[collName].insert({_id: 0}));
        assert.commandWorked(this.testDB.createView(viewName, collName, [{$match: {_id: 0}}]));

        this.assertCommandsFailWithShardError = (failCommandName, nss) => {
            const fp = configureFailPoint(this.st.rs0.getPrimary(), "failCommand", {
                failCommands: [failCommandName],
                failInternalCommands: true,
                namespace: dbName,
                errorCode: ErrorCodes.InternalError,
                errorMsg: errorMsg,
            });
            try {
                for (const cmd of commandsOn(nss)) {
                    const res = assert.commandFailedWithCode(
                        this.testDB.runCommand(cmd),
                        ErrorCodes.InternalError,
                        {cmd},
                    );
                    assert(res.errmsg.includes(errorMsg), "expected the primary shard's error", {
                        cmd,
                        res,
                    });
                }
            } finally {
                fp.off();
            }
        };
    });

    after(function () {
        this.st.stop();
        this.mongotMock.stop();
    });

    it("returns the shard's listCollections error for a collection", function () {
        this.assertCommandsFailWithShardError("listCollections", collName);
    });

    it("returns the shard's listCollections error for a view", function () {
        this.assertCommandsFailWithShardError("listCollections", viewName);
    });

    it("returns the shard's _shardsvrResolveView error for a view", function () {
        this.assertCommandsFailWithShardError("_shardsvrResolveView", viewName);
    });
});
