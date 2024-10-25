/**
 * Tests that diagnostic information about $search queries is reported in the profiler and slow
 * query log output.
 * @tags: [
 *     # Profiling is not supported on mongos.
 *     assumes_against_mongod_not_mongos
 * ]
 */
import {resultsEq} from "jstests/aggregation/extras/utils.js";
import {findMatchingLogLine} from "jstests/libs/log.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const collName = "search_profiling";
const coll = db[collName];
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0, size: "small"},
    {_id: 1, size: "medium", mood: "content hippo"},
    {_id: 2, size: "large", mood: "very hungry hippo"}
]));

createSearchIndex(coll, {name: "test-dynamic", definition: {"mappings": {"dynamic": true}}});

const searchForHungryHippo = {
    $search: {
        index: "test-dynamic",
        text: {query: "hungry hippo", path: ["mood"]},
    }
};

// Enable profiling on the database.
db.setProfilingLevel(1, {slowms: -1});

// Run a $search query.
const queryComment = "profiling query on " + collName;
let results = coll.aggregate([searchForHungryHippo], {comment: queryComment}).toArray();
assert(resultsEq(
    [
        {_id: 2, size: "large", mood: "very hungry hippo"},
        {_id: 1, size: "medium", mood: "content hippo"}
    ],
    results));

const unionWithQueryComment = "profiling unionWith query on " + collName;
results = coll.aggregate(
                  [
                      searchForHungryHippo,
                      {$unionWith: {coll: coll.getName(), pipeline: [searchForHungryHippo]}}
                  ],
                  {comment: unionWithQueryComment})
              .toArray();
assert(resultsEq(
    [
        {_id: 2, size: "large", mood: "very hungry hippo"},
        {_id: 1, size: "medium", mood: "content hippo"},
        {_id: 2, size: "large", mood: "very hungry hippo"},
        {_id: 1, size: "medium", mood: "content hippo"},
    ],
    results));

const lookupQueryComment = "profiling lookup query on " + collName;
results = coll.aggregate(
                        [
                            searchForHungryHippo,
                            {$lookup: {from: coll.getName(), pipeline: [searchForHungryHippo, {$project: {_id: "$_id"}}], as: "docs"}}
                        ],
                        {comment: lookupQueryComment})
                    .toArray();
assert(resultsEq(
    [
        {_id: 2, size: "large", mood: "very hungry hippo", docs: [{_id: 2}, {_id: 1}]},
        {_id: 1, size: "medium", mood: "content hippo", docs: [{_id: 2}, {_id: 1}]},
    ],
    results));

// Check the slow query log for our $search queries.
function checkLog(log, comment, ndocs) {
    let slowQueryLog = findMatchingLogLine(log, {msg: "Slow query", comment: comment});
    assert(slowQueryLog, log);
    slowQueryLog = JSON.parse(slowQueryLog);

    assert.eq(slowQueryLog.attr.keysExamined, ndocs, slowQueryLog);
    assert.eq(slowQueryLog.attr.docsExamined, ndocs, slowQueryLog);
    assert(slowQueryLog.attr.hasOwnProperty("mongot"), slowQueryLog);
}
const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
checkLog(log, queryComment, 2);
checkLog(log, unionWithQueryComment, 4);
checkLog(log, lookupQueryComment, 4);

// Check system.profile for our $search queries.
function checkProfiler(comment, ndocs) {
    const profileEntry = db.system.profile.findOne({'command.comment': comment});
    assert(profileEntry, db.system.profile.find({}, {command: 1}).toArray());
    assert.eq(profileEntry.keysExamined, ndocs, profileEntry);
    assert.eq(profileEntry.docsExamined, ndocs, profileEntry);
}
checkProfiler(queryComment, 2);
checkProfiler(unionWithQueryComment, 4);
checkProfiler(lookupQueryComment, 4);
dropSearchIndex(coll, {name: "test-dynamic"});