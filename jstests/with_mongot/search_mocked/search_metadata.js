/**
 * Tests that "searchScore", "searchHighlights", and "searchScoreDetails" metadata is properly
 * plumbed through the $search agg stage.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForQuery,
    MongotMock,
    mongotResponseForBatch
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const dbName = "test";

// Start mock mongot.
const mongotMock = new MongotMock();
mongotMock.start();
const mockConn = mongotMock.getConnection();

// Start mongod.
const conn = MongoRunner.runMongod({
    setParameter: {mongotHost: mockConn.host},
});
const testDB = conn.getDB(dbName);
const coll = testDB.search_metadata;

assert.commandWorked(coll.insert({_id: 0, foo: 1, bar: "projected out"}));
assert.commandWorked(coll.insert({_id: 1, foo: 2, bar: "projected out"}));
assert.commandWorked(coll.insert({_id: 10, foo: 3, bar: "projected out"}));
assert.commandWorked(coll.insert({_id: 11, foo: 4, bar: "projected out"}));
assert.commandWorked(coll.insert({_id: 20, foo: 5, bar: "projected out"}));
const collUUID = getUUIDFromListCollections(testDB, coll.getName());

// $search populates {$meta: "searchScore"}, {$meta: "searchHighlights"}, and {$meta:
// "searchScoreDetails"}.
{
    const mongotQuery = {scoreDetails: true};
    const cursorId = NumberLong(123);
    const pipeline = [
        {$search: mongotQuery},
        {
            $project: {
                _id: 1,
                foo: 1,
                score: {$meta: "searchScore"},
                highlights: {$meta: "searchHighlights"},
                scoreInfo: {$meta: "searchScoreDetails"}
            }
        }
    ];
    const highlights = ["a", "b", "c"];
    const searchScoreDetails = {value: 1.234, description: "the score is great", details: []};
    const mongotResponseBatch = [{
        _id: 0,
        $searchScore: 1.234,
        $searchHighlights: highlights,
        $searchScoreDetails: searchScoreDetails
    }];
    const responseOk = 1;
    const expectedDocs =
        [{_id: 0, foo: 1, score: 1.234, highlights: highlights, scoreInfo: searchScoreDetails}];

    const history = [{
        expectedCommand: mongotCommandForQuery(
            {query: mongotQuery, collName: coll.getName(), db: dbName, collectionUUID: collUUID}),
        response: mongotResponseForBatch(
            mongotResponseBatch, NumberLong(0), coll.getFullName(), responseOk),
    }];
    mongotMock.setMockResponses(history, cursorId);
    assert.eq(coll.aggregate(pipeline).toArray(), expectedDocs);
}

// Check that metadata is passed along correctly when there are multiple batches, both between
// the shell and mongod, and between mongod and mongot.
{
    const mongotQuery = {scoreDetails: true};
    const cursorId = NumberLong(123);
    const pipeline = [
        {$search: mongotQuery},
        {
            $project: {
                _id: 1,
                foo: 1,
                score: {$meta: "searchScore"},
                highlights: {$meta: "searchHighlights"},
                scoreInfo: {$meta: "searchScoreDetails"}
            }
        }
    ];

    function searchScoreDetailsFirstBatch(searchScore) {
        return {value: searchScore, description: "the score is very good", details: []};
    }
    const batchOne = [
        {
            _id: 0,
            $searchScore: 1.234,
            $searchHighlights: ["a"],
            $searchScoreDetails: searchScoreDetailsFirstBatch(1.234)
        },
        {
            _id: 1,
            $searchScore: 1.21,
            $searchHighlights: ["a", "b", "c"],
            $searchScoreDetails: searchScoreDetailsFirstBatch(1.21)
        }
    ];

    function searchScoreDetailsSecondBatch(searchScore) {
        return {value: searchScore, description: "the score is fantastic", details: []};
    }
    const batchTwo = [
        {
            _id: 10,
            $searchScore: 0.0,
            // Missing $searchHighlights
            $searchScoreDetails: searchScoreDetailsSecondBatch(0.0)
        },
        {
            _id: 11,
            $searchScore: 0.1,
            // Empty $searchHighlights.
            $searchHighlights: [],
            $searchScoreDetails: searchScoreDetailsSecondBatch(0.1)
        },
    ];

    // searchHighlights should be able to be an array of objects.
    function searchScoreDetailsThirdBatch(searchScore) {
        return {value: searchScore, description: "the score could not be better", details: []};
    }
    const highlightsWithSubobjs = [{a: 1, b: 1}, {a: 1, b: 2}];
    const batchThree = [{
        _id: 20,
        $searchScore: 0.2,
        $searchHighlights: highlightsWithSubobjs,
        $searchScoreDetails: searchScoreDetailsThirdBatch(0.2)
    }];

    const expectedDocs = [
        {
            _id: 0,
            foo: 1,
            score: 1.234,
            highlights: ["a"],
            scoreInfo: searchScoreDetailsFirstBatch(1.234)
        },
        {
            _id: 1,
            foo: 2,
            score: 1.21,
            highlights: ["a", "b", "c"],
            scoreInfo: searchScoreDetailsFirstBatch(1.21)
        },
        {_id: 10, foo: 3, score: 0.0, scoreInfo: searchScoreDetailsSecondBatch(0.0)},
        {
            _id: 11,
            foo: 4,
            score: 0.1,
            highlights: [],
            scoreInfo: searchScoreDetailsSecondBatch(0.1)
        },
        {
            _id: 20,
            foo: 5,
            score: 0.2,
            highlights: highlightsWithSubobjs,
            scoreInfo: searchScoreDetailsThirdBatch(0.2)
        },
    ];

    const history = [
        {
            expectedCommand: mongotCommandForQuery({
                query: mongotQuery,
                collName: coll.getName(),
                db: dbName,
                collectionUUID: collUUID
            }),
            response: mongotResponseForBatch(batchOne, cursorId, coll.getFullName(), 1),
        },
        {
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response: mongotResponseForBatch(batchTwo, cursorId, coll.getFullName(), 1),
        },
        {
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response: mongotResponseForBatch(batchThree, NumberLong(0), coll.getFullName(), 1),
        }
    ];
    mongotMock.setMockResponses(history, cursorId);
    assert.eq(coll.aggregate(pipeline, {cursor: {batchSize: 2}}).toArray(), expectedDocs);
}

// Check null metadata is handled properly.
{
    const searchInSbe = checkSbeRestrictedOrFullyEnabled(testDB) &&
        FeatureFlagUtil.isPresentAndEnabled(testDB.getMongo(), 'SearchInSbe');
    const mongotQuery = {scoreDetails: true};
    const cursorId = NumberLong(123);
    const pipeline = [
        {$search: mongotQuery},
        {
            $project: {
                _id: 1,
                foo: 1,
                score: {$meta: "searchScore"},
                highlights: {$meta: "searchHighlights"},
                scoreInfo: {$meta: "searchScoreDetails"}
            }
        }
    ];

    function searchScoreDetailsFirstBatch(searchScore) {
        return {value: searchScore, description: "the score is very good", details: []};
    }
    const response1 = [{
        _id: 1,
        $searchScore: null,
        $searchHighlights: [],
        $searchScoreDetails: searchScoreDetailsFirstBatch(1.21)
    }];

    const makeHistory = (response) => [{
        expectedCommand: mongotCommandForQuery(
            {query: mongotQuery, collName: coll.getName(), db: dbName, collectionUUID: collUUID}),
        response: mongotResponseForBatch(response, NumberLong(0), coll.getFullName(), 1),
    },
    ];
    mongotMock.setMockResponses(makeHistory(response1), cursorId);
    assert.throwsWithCode(() => coll.aggregate(pipeline), [7856601, 8107800, 13111]);

    const response2 = [{
        _id: 1,
        $searchScore: 0.1,
        $searchHighlights: null,
        $searchScoreDetails: searchScoreDetailsFirstBatch(1.21)
    }];

    mongotMock.setMockResponses(makeHistory(response2), cursorId);
    if (searchInSbe) {
        assert.throwsWithCode(() => coll.aggregate(pipeline), [7856602, 8107800]);
    } else {
        const expectedDoc = [{
            _id: 1,
            foo: 2,
            score: 0.1,
            highlights: null,
            scoreInfo: searchScoreDetailsFirstBatch(1.21)
        }];
        assert.eq(coll.aggregate(pipeline, {cursor: {batchSize: 2}}).toArray(), expectedDoc);
    }

    const response3 =
        [{_id: 1, $searchScore: 0.1, $searchHighlights: [], $searchScoreDetails: null}];

    mongotMock.setMockResponses(makeHistory(response3), cursorId);
    assert.throwsWithCode(() => coll.aggregate(pipeline), [7856603, 8107800, 10065]);
}

mongotMock.stop();
MongoRunner.stopMongod(conn);
