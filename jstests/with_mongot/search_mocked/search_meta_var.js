/**
 * Verify the behavior of the '$$SEARCH_META' variable in aggregation sub-pipelines.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const db = conn.getDB("test");
const coll = db.searchCollector;
coll.drop();

assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));

const collUUID = getUUIDFromListCollections(db, coll.getName());
const searchQuery = {
    query: "cakes",
    path: "title"
};

const searchCmd = {
    search: coll.getName(),
    collectionUUID: collUUID,
    query: searchQuery,
    $db: "test"
};

// Give mongotmock some stuff to return.
var cursorId = NumberLong(123);

function setupMocks(searchMetaValue) {
    const history = [{
        expectedCommand: searchCmd,
        response: {
            ok: 1,
            cursor: {
                id: NumberLong(0),
                ns: coll.getFullName(),
                nextBatch: [
                    {_id: 2, $searchScore: 0.654},
                    {_id: 1, $searchScore: 0.321},
                    {_id: 3, $searchScore: 0.123}
                ]
            },
            vars: {SEARCH_META: {value: searchMetaValue}}
        }
    }];

    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
    cursorId = NumberLong(cursorId + 1);
}

// $search present in the “same pipeline” but only before a sub-pipeline.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$search: searchQuery},
        {$unionWith: {pipeline: [{$documents: [{a: 1}]}]}},
        {$project: {_id: 1, meta: "$$SEARCH_META"}}
    ],
    cursor: {}
}),
                             6347901);

setupMocks(17);
setupMocks(19);
// $search present in local and later child pipeline.
assert.sameMembers(
    coll.aggregate(
            [
                {$search: searchQuery},
                {$project: {_id: 1, meta: "$$SEARCH_META"}},
                {
                    $unionWith: {
                        coll: coll.getName(),
                        pipeline: [
                            {$search: searchQuery},
                            {$project: {_id: {$add: [100, "$_id"]}, meta: "$$SEARCH_META"}},
                        ]
                    }
                }
            ],
            {cursor: {}})
        .toArray(),
    [
        {"_id": 2, "meta": {"value": 17}},
        {"_id": 1, "meta": {"value": 17}},
        {"_id": 3, "meta": {"value": 17}},
        {"_id": 102, "meta": {"value": 19}},
        {"_id": 101, "meta": {"value": 19}},
        {"_id": 103, "meta": {"value": 19}},
    ]);

// $search present in local and earlier child pipeline.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$search: searchQuery},
        {
            $unionWith: {
                coll: coll.getName(),
                pipeline: [
                    {$search: searchQuery},
                ]
            }
        },
        {$project: {meta: "$$SEARCH_META"}}
    ],
    cursor: {}
}),
                             6347901);

setupMocks(17);
setupMocks(19);
// $search present in local and parent pipeline.
assert.sameMembers(
    coll.aggregate(
            [
                {$search: searchQuery},
                {$project: {_id: 1}},
                {
                    $unionWith: {
                        coll: coll.getName(),
                        pipeline: [
                            {$search: searchQuery},
                            {$project: {_id: {$add: [100, "$_id"]}, meta: "$$SEARCH_META"}},
                        ]
                    }
                }
            ],
            {cursor: {}})
        .toArray(),
    [
        {"_id": 2},
        {"_id": 1},
        {"_id": 3},
        {"_id": 102, "meta": {"value": 19}},
        {"_id": 101, "meta": {"value": 19}},
        {"_id": 103, "meta": {"value": 19}},
    ]);

setupMocks(17);
setupMocks(18);
let expected = [];
let cursor = 17;

for (let i = 1; i < 4; i++) {
    expected.push({
        "_id": i,
        "lookup1": [
            {"_id": 2, "meta": {value: cursor}},
            {"_id": 1, "meta": {value: cursor}},
            {"_id": 3, "meta": {value: cursor}},
        ],
        "lookup2": [
            {"_id": 2, "meta": {value: cursor + 1}},
            {"_id": 1, "meta": {value: cursor + 1}},
            {"_id": 3, "meta": {value: cursor + 1}},
        ],
    });
    // We reset cursor to 17 here to generate the correct expected results. $lookup only executes
    // $search once since its values are cached. "lookup1" results are associated with cursor 17
    // and "lookup2" with cursor 18.
    cursor = 17;
}
// $search present only in parent pipeline but carried through correlated $lookup subquery.
assert.sameMembers(coll.aggregate(
        [
            {$project: {_id: 1}},
            {
                $lookup: {
                    from: coll.getName(),
                    pipeline: [
                        {$search: searchQuery},
                        {$project: {meta: "$$SEARCH_META"}}
                    ],
                    as: "lookup1"

                }
            },
            {
                $lookup: {
                    from: coll.getName(),
                    pipeline: [
                        {$search: searchQuery},
                        {$project: {meta: "$$SEARCH_META"}}
                    ],
                    as: "lookup2"

                }
            }
        ],
        {cursor: {}}
    ).toArray(),
    expected);

// $search is present only in an earlier child pipeline.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$match: {}},
        {
            $unionWith: {
                coll: coll.getName(),
                pipeline: [
                    {$search: searchQuery},
                ]
            }
        },
        {$project: {meta: "$$SEARCH_META"}}
    ],
    cursor: {}
}),
                             6347901);

// $search is present only in an earlier child pipeline.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$project: {meta: "$$SEARCH_META"}},
        {
            $unionWith: {
                coll: coll.getName(),
                pipeline: [
                    {$search: searchQuery},
                ]
            }
        }
    ],
    cursor: {}
}),
                             6347902);

// $search present only in an earlier parent pipeline.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$search: searchQuery},
        {$unionWith: {coll: coll.getName(), pipeline: [{$project: {meta: "$$SEARCH_META"}}]}}
    ],
    cursor: {}
}),
                             6347902);

setupMocks(17);
// $search present only in parent pipeline but carried through correlated $lookup subquery.
assert.sameMembers(coll.aggregate(
        [
            {$search: searchQuery},
            {$project: {_id: 1}},
            {
                $lookup: {
                    let: {mySearchMeta: "$$SEARCH_META"},
                    pipeline: [
                        {$documents: [{a: "$$mySearchMeta"}]}
                    ],
                    as: "lookup"

                }
            }
        ],
        {cursor: {}}
    ).toArray(),
    [
        {"_id": 2, "lookup": [{a: {value: 17}}]},
        {"_id": 1, "lookup": [{a: {value: 17}}]},
        {"_id": 3, "lookup": [{a: {value: 17}}]},
    ]);

MongoRunner.stopMongod(conn);
mongotmock.stop();
