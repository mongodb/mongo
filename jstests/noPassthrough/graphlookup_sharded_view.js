// Test that sharded $graphLookup can resolve sharded views correctly.
// @tags: [requires_sharding, requires_fcv_51]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/libs/log.js");                  // For findMatchingLogLines.

const sharded = new ShardingTest({
    mongos: 1,
    shards: [{verbose: 3}, {verbose: 3}, {verbose: 3}],
    config: 1,
});
assert(sharded.adminCommand({enableSharding: "test"}));

const isShardedLookupEnabled =
    sharded.s.adminCommand({getParameter: 1, featureFlagShardedLookup: 1})
        .featureFlagShardedLookup.value;
if (!isShardedLookupEnabled) {
    sharded.stop();
    return;
}

const testDBName = "test";
const testDB = sharded.getDB(testDBName);
sharded.ensurePrimaryShard(testDBName, sharded.shard0.shardName);

// Create 'docs' collection which will be backed by shard 1 from which we will run aggregations.
const docs = testDB.docs;
docs.drop();
const docsDocs = [
    {_id: 1, shard_key: "shard1", name: "Carter", subject: "Astrophysics"},
    {_id: 2, shard_key: "shard1", name: "Jackson", subject: "Archaeology"},
    {_id: 3, shard_key: "shard1", name: "Jones", subject: "Archaeology"},
    {_id: 4, shard_key: "shard1", name: "Mann", subject: "Theoretical Physics"},
    {_id: 5, shard_key: "shard1", name: "Mann", subject: "Xenobiology"},
];
assert.commandWorked(docs.createIndex({shard_key: 1}));
assert.commandWorked(docs.insertMany(docsDocs));
assert(sharded.s.adminCommand({shardCollection: docs.getFullName(), key: {shard_key: 1}}));

// Create first 'subjects' collection which will be backed by shard 2.
const subjects = testDB.subjects;
subjects.drop();
const subjectsDocs = [
    {_id: 1, shard_key: "shard2", name: "Science"},
    {_id: 2, shard_key: "shard2", name: "Biology", parent: "Science"},
    {_id: 3, shard_key: "shard2", name: "Physics", parent: "Science"},
    {_id: 4, shard_key: "shard2", name: "Anthropology", parent: "Humanities"},
    {_id: 5, shard_key: "shard2", name: "Astrophysics", parent: "Physics"},
    {_id: 6, shard_key: "shard2", name: "Archaeology", parent: "Anthropology"},
    {_id: 7, shard_key: "shard2", name: "Theoretical Physics", parent: "Physics"},
    {_id: 8, shard_key: "shard2", name: "Xenobiology", parent: "Biology"},
    {_id: 9, shard_key: "shard2", name: "Humanities"},
];
assert.commandWorked(subjects.createIndex({shard_key: 1}));
assert.commandWorked(subjects.insertMany(subjectsDocs));
assert(sharded.s.adminCommand({shardCollection: subjects.getFullName(), key: {shard_key: 1}}));

let testCount = 0;
function getMatchingLogsForTestRun(logs, fields) {
    let foundTest = false;

    // Filter out any logs that happened before the current aggregation.
    function getLogsForTestRun(log) {
        if (foundTest) {
            return true;
        }
        const m = findMatchingLogLine([log], {comment: "test " + testCount});
        if (m !== null) {
            foundTest = true;
        }
        return foundTest;
    }

    // Pick only those remaining logs which match the input 'fields'.
    return [...findMatchingLogLines(logs.filter(getLogsForTestRun), fields)];
}

function getShardedViewExceptions(shard) {
    const shardLog = assert.commandWorked(sharded[shard].adminCommand({getLog: "global"})).log;
    return ["test.docs", "test.subjects"].map(ns => {
        return {ns: ns, count: [...getMatchingLogsForTestRun(shardLog, {id: 5865400, ns})].length};
    });
}

function testGraphLookupView({collection, pipeline, expectedResults, expectedExceptions}) {
    assertArrayEq({
        actual: collection.aggregate(pipeline, {comment: "test " + testCount}).toArray(),
        expected: expectedResults
    });
    if (expectedExceptions) {
        // Count how many CommandOnShardedViewNotSupported exceptions we get and verify that they
        // match the number we were expecting.
        const exceptionCounts = getShardedViewExceptions(expectedExceptions.shard);
        for (const actualEx of exceptionCounts) {
            const ns = actualEx.ns;
            const actualCount = actualEx.count;
            const expectedCount = expectedExceptions[ns];
            assert(actualCount == expectedCount,
                   "expected: " + expectedCount + " exceptions for ns " + ns + ", actually got " +
                       actualCount + " exceptions.");
        }
    }
    testCount++;
}

function checkView(viewName, expected) {
    assertArrayEq({actual: testDB[viewName].find({}).toArray(), expected});
}

function moveChunksByShardKey(collection, shard) {
    assert.commandWorked(testDB.adminCommand({
        moveChunk: collection.getFullName(),
        find: {shard_key: shard},
        to: sharded[shard].shardName,
        _waitForDelete: true
    }));
}

// In order to trigger CommandOnShardedViewNotSupportedOnMongod exceptions where a shard cannot
// resolve a view definition, ensure that:
// - 'docs' is backed by shard 1.
// - 'subjects' is backed by shard 2.
moveChunksByShardKey(docs, "shard1");
moveChunksByShardKey(subjects, "shard2");

// Create a view with an empty pipeline on 'subjects'.
assert.commandWorked(testDB.createView("emptyViewOnSubjects", subjects.getName(), []));
checkView("emptyViewOnSubjects", subjectsDocs);

// Test a $graphLookup that triggers a CommandOnShardedViewNotSupportedOnMongod exception for a view
// with an empty pipeline.
testGraphLookupView({
    collection: docs,
    pipeline: [
        {$graphLookup: {
            from: "emptyViewOnSubjects",
            startWith: "$subject",
            connectFromField: "parent",
            connectToField: "name",
            as: "subjects",
        }},
        {$project: {
            name: 1,
            subjects: "$subjects.name"
        }}
    ],
    expectedResults: [
        {_id: 1, name: "Carter", subjects: ["Astrophysics", "Physics", "Science"]},
        {_id: 2, name: "Jackson", subjects: ["Anthropology", "Archaeology", "Humanities"]},
        {_id: 3, name: "Jones", subjects: ["Anthropology", "Archaeology", "Humanities"]},
        {_id: 4, name: "Mann", subjects: ["Physics", "Science", "Theoretical Physics"]},
        {_id: 5, name: "Mann", subjects: ["Biology", "Science", "Xenobiology"]},
    ],
    // Expect only one exception when trying to resolve the view 'emptyViewOnSubjects'.
    expectedExceptions: {"shard": "shard1", "test.docs": 0, "test.subjects": 1},
});

// Test a $graphLookup with a restrictSearchWithMatch that triggers a
// CommandOnShardedViewNotSupportedOnMongod exception for a view with an empty pipeline.
testGraphLookupView({
    collection: docs,
    pipeline: [
        {$graphLookup: {
            from: "emptyViewOnSubjects",
            startWith: "$subject",
            connectFromField: "parent",
            connectToField: "name",
            as: "subjects",
            restrictSearchWithMatch: { "name" : {$nin: ["Anthropology", "Archaeology", "Humanities"]} }
        }},
        {$project: {
            name: 1,
            science: {$gt: [{$size: "$subjects"}, 0]}
        }}
    ],
    expectedResults: [
        {_id: 1, name: "Carter", science: true},
        {_id: 2, name: "Jackson",  science: false},
        {_id: 3, name: "Jones",  science: false},
        {_id: 4, name: "Mann", science: true},
        {_id: 5, name: "Mann",  science: true},
    ],
    // Expect only one exception when trying to resolve the view 'emptyViewOnSubjects'.
    expectedExceptions: {"shard": "shard1", "test.docs": 0, "test.subjects": 1},
});

// Create a view with an empty pipeline on the existing empty view on 'subjects'.
assert.commandWorked(
    testDB.createView("emptyViewOnViewOnSubjects", testDB.emptyViewOnSubjects.getName(), []));
checkView("emptyViewOnViewOnSubjects", subjectsDocs);

// Test a $graphLookup that triggers a CommandOnShardedViewNotSupportedOnMongod exception for a view
// on another view.
testGraphLookupView({
    collection: docs,
    pipeline: [
        {$graphLookup: {
            from: "emptyViewOnViewOnSubjects",
            startWith: "$subject",
            connectFromField: "parent",
            connectToField: "name",
            as: "subjects",
        }},
        {$project: {
            name: 1,
            subjects: "$subjects.name"
        }}
    ],
    expectedResults: [
        {_id: 1, name: "Carter", subjects: ["Astrophysics", "Physics", "Science"]},
        {_id: 2, name: "Jackson", subjects: ["Anthropology", "Archaeology", "Humanities"]},
        {_id: 3, name: "Jones", subjects: ["Anthropology", "Archaeology", "Humanities"]},
        {_id: 4, name: "Mann", subjects: ["Physics", "Science", "Theoretical Physics"]},
        {_id: 5, name: "Mann", subjects: ["Biology", "Science", "Xenobiology"]},
    ],
    // Expect only one exception when trying to resolve the view 'emptyViewOnSubjects'.
    expectedExceptions: {"shard": "shard1", "test.docs": 0, "test.subjects": 1},
});

// Create a view with a pipeline on 'docs' that runs another $graphLookup.
assert.commandWorked(testDB.createView("physicists", docs.getName(), [
        {$graphLookup: {
            from: "emptyViewOnSubjects",
            startWith: "$subject",
            connectFromField: "parent",
            connectToField: "name",
            as: "subjects",
        }},
        {$match: {
            "subjects.name": "Physics"
        }},
        {$project: {
            name: 1,
            specialty: "$subject",
            subjects: "$subjects.name"
        }}
]));
checkView("physicists", [
    {
        _id: 1,
        name: "Carter",
        specialty: "Astrophysics",
        subjects: ["Astrophysics", "Physics", "Science"]
    },
    {
        _id: 4,
        name: "Mann",
        specialty: "Theoretical Physics",
        subjects: ["Physics", "Science", "Theoretical Physics"]
    },
]);

// Test a $graphLookup that triggers a CommandOnShardedViewNotSupportedOnMongod exception for a view
// with a pipeline that contains a $graphLookup.
testGraphLookupView({
    collection: subjects,
    pipeline: [
        {$graphLookup: {
            from: "physicists",
            startWith: "$name",
            connectFromField: "subjects",
            connectToField: "specialty",
            as: "practitioner",
        }},
        {$unwind: "$practitioner"},
    ],
    expectedResults: [
        {_id: 5, shard_key: "shard2", name: "Astrophysics", parent: "Physics",
         practitioner: {_id: 1, name: "Carter", specialty: "Astrophysics",
         subjects: ["Astrophysics", "Physics", "Science"]}},
        {_id: 7, shard_key: "shard2", name: "Theoretical Physics", parent: "Physics",
        practitioner: {_id: 4, name: "Mann", specialty: "Theoretical Physics",
        subjects: ["Physics", "Science", "Theoretical Physics"]}},
    ],
    // Expect one exception when trying to resolve the view 'physicists' on collection 'docs' and
    // another four on 'subjects' when trying to resolve 'emptyViewOnSubjects'.
    expectedExceptions: {"shard": "shard2", "test.docs": 1, "test.subjects": 4},
});

// Create a view with a pipeline on 'physicists' to test resolution of a view on another view.
assert.commandWorked(testDB.createView("physicist", testDB.physicists.getName(), [
    {$match: {"specialty": "Astrophysics"}},
]));

// Test a $graphLookup that triggers a CommandOnShardedViewNotSupportedOnMongod exception for a view
// on another view.
testGraphLookupView({
    collection: subjects,
    pipeline: [
        {$graphLookup: {
            from: "physicist",
            startWith: "$name",
            connectFromField: "subjects",
            connectToField: "specialty",
            as: "practitioner",
        }},
        {$unwind: "$practitioner"},
    ],
    expectedResults: [
        {_id: 5, shard_key: "shard2", name: "Astrophysics", parent: "Physics",
         practitioner: {_id: 1, name: "Carter", specialty: "Astrophysics",
         subjects: ["Astrophysics", "Physics", "Science"]}},
    ],
    // Expect one exception when trying to resolve the view 'physicists' on collection 'docs' and
    // one on 'subjects' when trying to resolve 'emptyViewOnSubjects'.
    expectedExceptions: {"shard": "shard2", "test.docs": 1, "test.subjects": 1},
});

// Test a $graphLookup with restrictSearchWithMatch that triggers a
// CommandOnShardedViewNotSupportedOnMongod exception for a view with a pipeline that contains a
// $graphLookup.
testGraphLookupView({
    collection: subjects,
    pipeline: [
        {$graphLookup: {
            from: "physicists",
            startWith: "$name",
            connectFromField: "subjects",
            connectToField: "specialty",
            as: "practitioner",
            restrictSearchWithMatch: { name: "Mann" }
        }},
        {$unwind: "$practitioner"},
    ],
    expectedResults: [
        {_id: 7, shard_key: "shard2", name: "Theoretical Physics", parent: "Physics",
         practitioner: {_id: 4, name: "Mann", specialty: "Theoretical Physics",
         subjects: ["Physics", "Science", "Theoretical Physics"]}},
    ],
    // Expect one exception when trying to resolve the view 'physicists' on collection 'docs' and
    // another two on 'subjects' when trying to resolve 'emptyViewOnSubjects'.
    expectedExceptions: {"shard": "shard2", "test.docs": 1, "test.subjects": 2},
});

sharded.stop();
}());
