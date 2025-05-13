/**
 * Tests the spilling statistics are a part of slow query logs
 *
 * @tags: [
 *     requires_persistence,
 *     requires_fcv_81,
 * ]
 */

import {findMatchingLogLine} from "jstests/libs/log.js";
import {code, linebreak, section, subSection} from "jstests/libs/pretty_md.js";
import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

function getServerParameter(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setServerParameter(knob, value) {
    assert.commandWorked(db.adminCommand({setParameter: 1, [knob]: value}));
}

function getSpillingAttrs(obj) {
    const spillingStats = {};
    for (let key of Object.keys(obj.attr).sort()) {
        if (key === "usedDisk" || key.endsWith("Spills") || key.endsWith("SpilledRecords")) {
            spillingStats[key] = obj.attr[key];
        } else if (key.endsWith("SpilledBytes") || key.endsWith("SpilledDataStorageSize")) {
            spillingStats[key] = "X";
        }
    }
    return spillingStats;
}

function outputPipelineAndSlowQueryLog(coll, pipeline, comment) {
    coll.aggregate(pipeline, {comment: comment}).itcount();
    const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
    const slowQueryLogLine =
        findMatchingLogLine(globalLog.log, {msg: "Slow query", comment: comment});
    assert(slowQueryLogLine, "Failed to find a log line matching the comment: " + comment);

    subSection("Pipeline");
    code(tojson(pipeline));

    subSection("Slow query spilling stats");
    code(tojson(getSpillingAttrs(JSON.parse(slowQueryLogLine))));

    linebreak();
}

function initTimeseriesColl(coll) {
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));
    const bucketMaxSpanSeconds =
        db.getCollectionInfos({name: coll.getName()})[0].options.timeseries.bucketMaxSpanSeconds;

    const batch = [];
    let batchTime = +(new Date());
    for (let j = 0; j < 50; ++j) {
        batch.push({time: new Date(batchTime), meta: 1});
        batchTime += bucketMaxSpanSeconds / 10;
    }
    assert.commandWorked(coll.insertMany(batch));
}

assert.commandWorked(db.setProfilingLevel(1, {slowms: -1}));
const coll = db[jsTestName()];
coll.drop();

const parametersToRestore = [];
function saveParameterToRestore(knob) {
    parametersToRestore.push({knob: knob, value: getServerParameter(knob)});
}

saveParameterToRestore("internalQueryMaxBlockingSortMemoryUsageBytes");

section("Sort with large memory limit");
setServerParameter("internalQueryMaxBlockingSortMemoryUsageBytes", 1000);
assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));
outputPipelineAndSlowQueryLog(coll, [{$sort: {a: 1}}], "sort with large memory limit");
coll.drop();

section("Sort with empty collection");
outputPipelineAndSlowQueryLog(coll, [{$sort: {a: 1}}], "sort with empty collection");

section("Sort with spilling");
setServerParameter("internalQueryMaxBlockingSortMemoryUsageBytes", 1);
assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}, {a: 3}]));
outputPipelineAndSlowQueryLog(coll, [{$sort: {a: 1}}], "one sort");
coll.drop();

section("Multiple sorts");
assert.commandWorked(coll.insertMany([{a: 1, b: 3}, {a: 2, b: 2}, {a: 3, b: 1}]));
outputPipelineAndSlowQueryLog(coll,
                              [{$sort: {a: 1}}, {$limit: 3}, {$sort: {b: 1}}],
                              "multiple sorts test: multiple sorts case");
coll.drop();

section("Timeseries sort");
initTimeseriesColl(coll);
outputPipelineAndSlowQueryLog(coll, [{$sort: {time: 1}}], "bounded sort on timeseries");
coll.drop();

saveParameterToRestore("internalDocumentSourceGroupMaxMemoryBytes");
saveParameterToRestore("internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill");

section("Group");

setServerParameter("internalDocumentSourceGroupMaxMemoryBytes", 1);
setServerParameter("internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill", 1);

assert.commandWorked(coll.insertMany([{a: 1, b: 1}, {a: 1, b: 2}, {a: 2, b: 1}, {a: 2, b: 2}]));
outputPipelineAndSlowQueryLog(coll,
                              [{$group: {_id: "$a", b: {$sum: "$b"}}}, {$sort: {b: 1}}],
                              "group and sort in a single pipeline");
coll.drop();

saveParameterToRestore("internalTextOrStageMaxMemoryBytes");

section("TextOr and projection");
setServerParameter("internalTextOrStageMaxMemoryBytes", 1);

assert.commandWorked(
    coll.insertMany([{a: "green tea", b: 5}, {a: "black tea", b: 6}, {a: "black coffee", b: 7}]));
assert.commandWorked(coll.createIndex({a: "text"}));

outputPipelineAndSlowQueryLog(
    coll,
    [{$match: {$text: {$search: "black tea"}}}, {$addFields: {score: {$meta: "textScore"}}}],
    "text or project meta");

section("TextOr and sort");
outputPipelineAndSlowQueryLog(
    coll,
    [{$match: {$text: {$search: "black tea"}}}, {$sort: {_: {$meta: "textScore"}}}],
    "text or sort on meta");

coll.drop();

saveParameterToRestore("internalDocumentSourceBucketAutoMaxMemoryBytes");
section("BucketAuto");
setServerParameter("internalDocumentSourceBucketAutoMaxMemoryBytes", 1);

assert.commandWorked(coll.insertMany([{a: 1, b: 1}, {a: 1, b: 2}, {a: 2, b: 1}, {a: 2, b: 2}]));
outputPipelineAndSlowQueryLog(
    coll, [{$bucketAuto: {groupBy: "$a", buckets: 2, output: {sum: {$sum: "$b"}}}}], "bucketAuto");

coll.drop();

saveParameterToRestore(
    "internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill");
section("HashLookup");
setServerParameter("internalQuerySlotBasedExecutionHashLookupApproxMemoryUseInBytesBeforeSpill", 1);

const students = db[jsTestName() + "_students"];
students.drop();
const people = db[jsTestName() + "_people"];
people.drop();

const studentsDocs = [
    {sID: 22001, name: "Alex", year: 1, score: 4.0},
    {sID: 21001, name: "Bernie", year: 2, score: 3.7},
    {sID: 20010, name: "Chris", year: 3, score: 2.5},
    {sID: 22021, name: "Drew", year: 1, score: 3.2},
    {sID: 17301, name: "Harley", year: 6, score: 3.1},
    {sID: 21022, name: "Alex", year: 2, score: 2.2},
    {sID: 20020, name: "George", year: 3, score: 2.8},
    {sID: 18020, name: "Harley", year: 5, score: 2.8},
];
const peopleDocs = [
    {pID: 1000, name: "Alex"},
    {pID: 1001, name: "Drew"},
    {pID: 1002, name: "Justin"},
    {pID: 1003, name: "Parker"},
];

assert.commandWorked(students.insertMany(studentsDocs));
assert.commandWorked(people.insertMany(peopleDocs));

outputPipelineAndSlowQueryLog(
    people,
    [{
        $lookup: {from: students.getName(), localField: "name", foreignField: "name", as: "matched"}
    }],
    "$lookup");

students.drop();
people.drop();

saveParameterToRestore("internalDocumentSourceGraphLookupMaxMemoryBytes");

section("Graph lookup");
setServerParameter("internalDocumentSourceGraphLookupMaxMemoryBytes", 1);

assert.commandWorked(coll.insertMany([
    {_id: 1, to: [2, 3]},
    {_id: 2, to: [4, 5]},
    {_id: 3, to: [6, 7]},
    {_id: 4},
    {_id: 5},
    {_id: 6},
    {_id: 7}
]));

const graphLookupStage = {
    $graphLookup:
        { from: "coll", startWith: 1, connectFromField: "to", connectToField: "_id", as: "path", depthField: "depth" }
};

outputPipelineAndSlowQueryLog(coll, [{$limit: 1}, graphLookupStage], "graph lookup");

section("Graph lookup with unwind and sort");
outputPipelineAndSlowQueryLog(
    coll,
    [{$limit: 1}, graphLookupStage, {$unwind: "$path"}, {$sort: {"path.depth": 1}}],
    "graph lookup unwind sort");

coll.drop();

section("HashLookupUnwind");

const locations = db[jsTestName() + "_locations"];
locations.drop();
const animals = db[jsTestName() + "_animals"];
animals.drop();

const locationsDocs = [
    {
        name: "doghouse",
        coordinates: [25.0, 60.0],
        extra: {breeds: ["terrier", "dachshund", "bulldog"]}
    },
    {
        _id: "bullpen",
        coordinates: [-25.0, -60.0],
        extra: {breeds: "Scottish Highland", feeling: "bullish"}
    },
    {
        name: "volcano",  // no animals are in this location, so no $lookup matches
        coordinates: [-1111.0, 2222.0],
        extra: {breeds: "basalt", feeling: "hot"}
    }
];
const animasDocs = [
    {_id: "dog", locationName: "doghouse", colors: ["chartreuse", "taupe"]},
    {_id: "bull", locationId: "bullpen", colors: ["red", "blue"]},
    {_id: "trout", colors: ["mauve"]},  // no "locationId" field, so no $lookup matches
];

assert.commandWorked(locations.insertMany(locationsDocs));
assert.commandWorked(animals.insertMany(animasDocs));

outputPipelineAndSlowQueryLog(
    animals,
    [
        {
            $lookup: {from: locations.getName(), localField: "locationName", foreignField: "name", as: "location"}
        },
        {$unwind: "$location"},
        {
            $project: {
                locationName: false,
                "location.extra": false,
                "location.coordinates": false,
                "colors": false
            }
        },
    ],
    "$lookup-$unwind");

locations.drop();
animals.drop();

saveParameterToRestore("internalDocumentSourceSetWindowFieldsMaxMemoryBytes");
section("SetWindowFields");

assert.commandWorked(coll.insertMany([{a: 1, b: 1}, {a: 1, b: 2}, {a: 2, b: 1}, {a: 2, b: 2}]));

const setWindowFieldsPipeline =
    [{$setWindowFields: {partitionBy: "$a", sortBy: {b: 1}, output: {sum: {$sum: "$b"}}}}];

function getSetWindowFieldsMemoryLimit() {
    const explain = coll.explain().aggregate(setWindowFieldsPipeline);
    // If $setWindowFields was pushed down to SBE, set a lower limit. We can't set it to 1 byte for
    // Classic because DocumentSourceSetWindowFields will fail if it still doesn't fit into memory
    // limit after spilling.
    if (getPlanStage(getWinningPlanFromExplain(explain), "WINDOW")) {
        return 1;
    } else {
        return 500;
    }
}

setServerParameter("internalDocumentSourceSetWindowFieldsMaxMemoryBytes",
                   getSetWindowFieldsMemoryLimit());

outputPipelineAndSlowQueryLog(coll, setWindowFieldsPipeline, "$setWindowFields");
outputPipelineAndSlowQueryLog(
    coll, setWindowFieldsPipeline.concat([{$limit: 1}]), "$setWindowFields + $limit");

coll.drop();

for (let restore of parametersToRestore) {
    setServerParameter(restore.knob, restore.value);
}
