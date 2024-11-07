/**
 * Test pipelines with $projects/$addFields mixed together with other stages, and verify these
 * pipelines produce correct results and correct plans.
 */
const coll = db.complex_pipelines;
const collName = db.getName();
const foreignColl = db.complex_pipelines_foreign;
const foreignCollName = foreignColl.getName();

coll.drop();
foreignColl.drop();

Random.setRandomSeed(20240725);

const indexSpec = {
    _id: 1,
    y: 1,
    x: 1,
    w: 1,
    l: 1,
    k: 1,
    j: 1,
    i: 1,
    h: 1,
    g: 1,
    f: 1,
    e: 1,
    d: 1,
    c: 1,
    b: 1,
    a: 1
};

let docs = [];
let foreignDocs = [];

function tojsoncompact(obj) {
    let str = tojsononeline(obj);
    str = str.replaceAll(' ', '').replaceAll('"', '');
    str = str.replaceAll(',', ', ').replaceAll(':', ': ');
    return str;
}

// Generate 8 documents in 'docs' and 8 documents in 'foreignDocs'.
for (let i = 1; i <= 8; ++i) {
    // Add a document to 'docs'. For each document, most field values are integers, except for
    // fields 'w', 'x', and 'y' which are objects (each with 3 subfields 'a', 'b', and 'c').
    let doc = {_id: i};

    let k = Random.randInt(11);
    let kStep = Random.randInt(10) + 1;
    for (let j = 0; j < 11; ++j) {
        if (k < 9) {
            let c = String.fromCharCode(97 + k);
            doc[c] = Random.randInt(100);
        } else {
            let c = String.fromCharCode(119 + k - 9);
            doc[c] = {a: Random.randInt(100), b: Random.randInt(100), c: Random.randInt(100)};
        }

        k = (k + kStep) % 11;
    }

    docs.push(doc);

    // Add a document to 'foreignDocs'.
    doc = {_id: i};

    k = Random.randInt(3);
    kStep = Random.randInt(2) + 1;
    for (let j = 0; j < 3; ++j) {
        let c = String.fromCharCode(97 + k);
        k = (k + kStep) % 3;

        doc[c] = Random.randInt(100);
    }

    foreignDocs.push(doc);
}

// possibleIntFields and possibleObjFields are global variables used in the buildXXX() methods below
// to pick the fields to be used in each operation. The two arrays should be shuffled using the
// shuffleArray method before each call to one of the addXXX() methods to randomise the order of the
// fields.
let possibleIntFields = [];
let possibleObjFields = [];

for (let j = 0; j < 12; ++j) {
    possibleIntFields.push(String.fromCharCode(97 + j));
}
for (let j = 0; j < 3; ++j) {
    possibleObjFields.push(String.fromCharCode(119 + j));
}

function shuffleArray(arr) {
    let i = arr.length;

    while (i != 0) {
        let j = Random.randInt(i);
        --i;

        let value = arr[i];
        arr[i] = arr[j];
        arr[j] = value;
    }
}

function addInclusion(pipeline, allowDottedPaths) {
    let projectionDoc = {};

    let n = 8 + Random.randInt(4);
    for (let j = 0; j < n; ++j) {
        let c = possibleIntFields[j];

        if (Random.rand() < 0.70) {
            projectionDoc[c] = 1;
        } else if (Random.rand() < 0.85) {
            let d = String.fromCharCode(97 + Random.randInt(12));
            projectionDoc[c] = "$" + d;
        } else {
            let num = Random.randInt(100);
            projectionDoc[c] = {"$literal": num};
        }
    }

    n = 1 + Random.randInt(2);
    for (let j = 0; j < n; ++j) {
        let c = possibleObjFields[j];

        if (Random.rand() < 0.5 || !allowDottedPaths) {
            projectionDoc[c] = 1;
        } else {
            let possibleSubfields = ["a", "b", "c"];
            shuffleArray(possibleSubfields);

            let m = 1 + Random.randInt(2);
            for (let i = 0; i < m; ++i) {
                let path = c + "." + possibleSubfields[i];
                projectionDoc[path] = 1;
            }
        }
    }

    return pipeline.concat([{$project: projectionDoc}]);
}

function addExclusion(pipeline, allowDottedPaths) {
    let projectionDoc = {};

    let n = 1 + Random.randInt(2);
    for (let j = 0; j < n; ++j) {
        let c = possibleIntFields[j];
        projectionDoc[c] = 0;
    }

    n = Random.randInt(2);
    for (let j = 0; j < n; ++j) {
        let c = possibleObjFields[j];

        if (Random.rand() < 0.5 || !allowDottedPaths) {
            projectionDoc[c] = 0;
        } else {
            let possibleSubfields = ["a", "b", "c"];
            shuffleArray(possibleSubfields);

            let m = 1 + Random.randInt(2);
            for (let i = 0; i < m; ++i) {
                let path = c + "." + possibleSubfields[i];
                projectionDoc[path] = 0;
            }
        }
    }

    return pipeline.concat([{$project: projectionDoc}]);
}

function addAddFields(pipeline, allowDottedPaths = true) {
    let addFieldsDoc = {};

    let n = 1 + Random.randInt(3);
    for (let j = 0; j < n; ++j) {
        let c = possibleIntFields[j];

        if (Random.rand() < 0.65) {
            let num = Random.randInt(100);
            addFieldsDoc[c] = num;
        } else {
            let d = String.fromCharCode(97 + Random.randInt(20));
            addFieldsDoc[c] = "$" + d;
        }
    }

    n = Random.randInt(2);
    for (let j = 0; j < n; ++j) {
        let c = possibleObjFields[j];

        if (Random.rand() < 0.5 || !allowDottedPaths) {
            let d = String.fromCharCode(119 + Random.randInt(3));

            addFieldsDoc[c] = "$" + d;
        } else {
            let possibleSubfields = ["a", "b", "c"];
            shuffleArray(possibleSubfields);

            let m = 1 + Random.randInt(2);
            for (let i = 0; i < m; ++i) {
                let path = c + "." + possibleSubfields[i];
                let num = Random.randInt(100);

                addFieldsDoc[path] = num;
            }
        }
    }

    return pipeline.concat([{$addFields: addFieldsDoc}]);
}

function addMatch(pipeline, allowDottedPaths = true) {
    let matchDoc = {};

    if (Random.rand() < 0.5 || !allowDottedPaths) {
        let c = possibleIntFields[0];
        let num = Random.randInt(25);
        let op = Random.randInt(4);

        if (op < 2) {
            num = 99 - num;
            matchDoc[c] = op == 0 ? {$lt: num} : {$lte: num};
        } else {
            matchDoc[c] = op == 2 ? {$gt: num} : {$gte: num};
        }
    } else {
        let possibleSubfields = ["a", "b", "c"];
        shuffleArray(possibleSubfields);

        let c = possibleObjFields[0];
        let path = c + "." + possibleSubfields[0];
        let num = Random.randInt(25);
        let op = Random.randInt(4);

        if (op < 2) {
            num = 99 - num;
            matchDoc[path] = op == 0 ? {$lt: num} : {$lte: num};
        } else {
            matchDoc[path] = op == 2 ? {$gt: num} : {$gte: num};
        }
    }

    return pipeline.concat([{$match: matchDoc}]);
}

function addGroup(pipeline) {
    let groupDoc = {};

    let g = String.fromCharCode(97 + Random.randInt(12));
    groupDoc["_id"] = "$" + g;

    let n = 8 + Random.randInt(4);
    for (let j = 0; j < n; ++j) {
        let c = possibleIntFields[j];
        let sumArg = "$" + c;
        groupDoc[c] = {$sum: sumArg};
    }

    for (let j = 0; j < 3; ++j) {
        let c = possibleObjFields[j];
        let minArg = "$" + c;
        groupDoc[c] = {$min: minArg};
    }

    return pipeline.concat([{$group: groupDoc}]);
}

function addLookupUnwind(pipeline) {
    let lookupDoc = {from: foreignCollName, localField: "_id", foreignField: "_id"};

    let c = String.fromCharCode(119 + Random.randInt(3));
    lookupDoc["as"] = c;

    let unwindArg = "$" + c;

    return pipeline.concat([{$lookup: lookupDoc}, {$unwind: unwindArg}]);
}

function addSort(pipeline, addLimit = false) {
    let sortDoc = {};

    let n = 1 + Random.randInt(2);
    for (let j = 0; j < n; ++j) {
        let c = possibleIntFields[j];
        sortDoc[c] = Random.randInt(2) == 0 ? 1 : -1;
    }

    // Add _id to make sure that sort order is always consistent.
    sortDoc["_id"] = 1;

    let newPipeline = pipeline.concat([{$sort: sortDoc}]);

    if (addLimit) {
        let num = 5 + Random.randInt(6);
        newPipeline = newPipeline.concat([{$limit: num}]);
    }

    return newPipeline;
}

function getRangeWindow() {
    switch (Random.randInt(6)) {
        case 0:
            return {documents: ["unbounded", "current"]};
        case 1:
            return {documents: ["current", "unbounded"]};
        case 2:
            return {documents: ["unbounded", "unbounded"]};
        case 3:
            return {documents: [-2, 0]};
        case 4:
            return {documents: [0, 2]};
        case 5:
            return {documents: [-2, 2]};
    }
}

function addSetWindowFields(pipeline) {
    let setWindowFieldsDoc = {};

    const partitionByIdx = Random.randInt(13);
    if (partitionByIdx < 12) {
        setWindowFieldsDoc["partitionBy"] = "$" + possibleIntFields[partitionByIdx];
    }

    const sortDoc = addSort([]);
    const sortByDoc = Object.values(sortDoc[0])[0];
    setWindowFieldsDoc["sortBy"] = sortByDoc;

    shuffleArray(possibleIntFields);
    shuffleArray(possibleObjFields);

    let outputDoc = {};

    const sumWindows = 1 + Random.randInt(2);
    const minWindows = Random.randInt(2);

    for (let j = 0; j < sumWindows; ++j) {
        const c = possibleIntFields[j];
        const arg = "$" + c;
        const windowDoc = getRangeWindow();

        outputDoc[c] = {$sum: arg, window: windowDoc};
    }

    for (let j = 0; j < minWindows; ++j) {
        const c = possibleObjFields[j];
        const arg = "$" + c;
        const windowDoc = getRangeWindow();
        outputDoc[c] = {$min: arg, window: windowDoc};
    }

    setWindowFieldsDoc["output"] = outputDoc;

    return pipeline.concat([{$setWindowFields: setWindowFieldsDoc}]);
}

function generateTestcase({
    allowInclusion,
    allowGroup,
    allowLookup,
    allowSort,
    allowSortWithLimit,
    allowSetWindowFields,
    allowDottedPaths
}) {
    // Initialize 'pipeline'.
    let pipeline = [];
    let numStages = Random.randInt(9) + 4;

    for (let i = 0; i < numStages;) {
        let r = Random.rand();
        shuffleArray(possibleIntFields);
        shuffleArray(possibleObjFields);

        if (r < 0.10) {
            if (allowInclusion !== true) {
                continue;
            }
            pipeline = addInclusion(pipeline, allowDottedPaths === true);
        } else if (r < 0.20) {
            pipeline = addExclusion(pipeline, allowDottedPaths === true);
        } else if (r < 0.40) {
            pipeline = addAddFields(pipeline, allowDottedPaths === true);
        } else if (r < 0.55) {
            pipeline = addMatch(pipeline, allowDottedPaths === true);
        } else if (r < 0.70) {
            if (allowGroup !== true) {
                continue;
            }

            pipeline = addGroup(pipeline);
        } else if (r < 0.80) {
            if (allowSetWindowFields !== true) {
                continue;
            }
            pipeline = addSetWindowFields(pipeline, allowDottedPaths === true);
        } else if (r < 0.90) {
            if (allowLookup !== true) {
                continue;
            }

            pipeline = addLookupUnwind(pipeline);
        } else {
            if (allowSort !== true) {
                continue;
            }

            if (r < 0.95 || allowSortWithLimit !== true) {
                pipeline = addSort(pipeline);
            } else {
                pipeline = addSort(pipeline, true);
            }
        }

        ++i;
    }

    return pipeline;
}

let testcases = [];
let testcaseId = 1;

for (let k = 0; k < 2; ++k) {
    let allowDottedPaths = k >= 1;

    for (let j = 0; j < 3; ++j) {
        let allowSort = j >= 1;
        let allowSortWithLimit = j >= 2;

        for (let i = 0; i < 12; ++i) {
            let allowInclusion = (i >= 3 && i < 6) || (i >= 9);
            let allowGroup = i >= 6;
            let allowLookup = i >= 9;
            let allowSetWindowFields = (i % 5 === 0);

            let testcase = generateTestcase({
                allowInclusion,
                allowGroup,
                allowLookup,
                allowSort,
                allowSortWithLimit,
                allowSetWindowFields,
                allowDottedPaths
            });

            testcases.push({id: testcaseId, pipeline: testcase});
            ++testcaseId;
        }
    }
}

print("Docs:\n\n");

for (let i = 0; i < docs.length; ++i) {
    let doc = docs[i];
    let comma = i != docs.length - 1 ? "," : "";
    print(tojsoncompact(doc) + comma);
}

print("\n\nForeign docs:\n\n");

for (let i = 0; i < foreignDocs.length; ++i) {
    let doc = foreignDocs[i];
    let comma = i != foreignDocs.length - 1 ? "," : "";
    print(tojsoncompact(doc) + comma);
}

print("\n\n");

function compareResultEntries(lhs, rhs) {
    const lhsJson = tojsononeline(lhs);
    const rhsJson = tojsononeline(rhs);
    return lhsJson < rhsJson ? -1 : (lhsJson > rhsJson ? 1 : 0);
}

function runTest(testcase, useIndex) {
    let testcaseId = testcase.id.toString();
    let pipeline = testcase.pipeline;

    let useIndexText = useIndex ? "true" : "false";
    print(`Query ${testcaseId} (with useIndex=${useIndexText}): ${tojsononeline(pipeline)}\n\n`);

    const options = useIndex ? {hint: indexSpec} : {};
    let results = coll.aggregate(pipeline, options).toArray();
    results.sort(compareResultEntries);

    for (let i = 0; i < results.length; ++i) {
        let result = results[i];
        let comma = i != results.length - 1 ? "," : "";
        print(tojsoncompact(result) + comma);
    }
    print("\n");
}

function runTests(useIndex) {
    for (let testcase of testcases) {
        runTest(testcase, useIndex);
    }
}

// Insert documents into 'coll' and 'foreignColl'.
assert.commandWorked(coll.insert(docs));
assert.commandWorked(foreignColl.insert(foreignDocs));

// Run all the testcases without an index.
let useIndex = false;
runTests(useIndex);

// Create an index.
assert.commandWorked(coll.createIndex(indexSpec));

// Run all the testcases _with_ an index.
useIndex = true;
runTests(useIndex);
