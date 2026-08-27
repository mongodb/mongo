/**
 * $search idLookup drops a missing object `_id` instead of tripwiring (SERVER-134017 / AF-20648).
 *
 * Real mongot only returns `_id`s it indexed, so this case stays on mongotmock: the mock can
 * emit an object `_id` that is not in the collection, interleaved with a scalar hit (SBE) and
 * a present object hit (local-read fallback).
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const searchQuery = {
    query: "query",
    path: "title",
};

describe("$search idLookup missing object `_id`", function () {
    before(function () {
        this.mongotmock = new MongotMock();
        this.mongotmock.start();
        this.mongotConn = this.mongotmock.getConnection();

        this.conn = MongoRunner.runMongod({
            setParameter: {
                mongotHost: this.mongotConn.host,
                featureFlagSearchOptimizedIdLookup: true,
            },
        });
        this.db = this.conn.getDB(jsTestName());
        this.coll = this.db.getCollection(jsTestName());
        this.present = {_id: {a: 1}, title: "present"};
        assert.commandWorked(this.coll.insert([{_id: 0, title: "scalar"}, this.present]));
    });

    after(function () {
        if (this.conn) {
            MongoRunner.stopMongod(this.conn);
        }
        if (this.mongotmock) {
            this.mongotmock.stop();
        }
    });

    it("drops a missing object `_id` without tripwiring", function () {
        assert.commandWorked(
            this.mongotConn.adminCommand({
                setMockResponses: 1,
                cursorId: NumberLong(1),
                history: [
                    {
                        expectedCommand: {
                            search: this.coll.getName(),
                            collectionUUID: getUUIDFromListCollections(
                                this.db,
                                this.coll.getName(),
                            ),
                            query: searchQuery,
                            $db: this.db.getName(),
                        },
                        response: {
                            cursor: {
                                id: NumberLong(0),
                                ns: this.coll.getFullName(),
                                nextBatch: [
                                    {_id: 0, $searchScore: 1},
                                    {_id: {missing: 1}, $searchScore: 0.9},
                                    {_id: {a: 1}, $searchScore: 0.8},
                                ],
                            },
                            ok: 1,
                        },
                    },
                ],
            }),
        );

        assertArrayEq({
            actual: this.coll.aggregate([{$search: searchQuery}]).toArray(),
            expected: [{_id: 0, title: "scalar"}, this.present],
        });
    });
});
