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

function addInclusion(testcase, allowDottedPaths) {
    let pipeline = testcase.pipeline;
    let fields = testcase.fields;

    let projectionDoc = {};
    let newFields = {};

    let possibleIntFields = [];
    let possibleObjFields = [];

    for (let j = 0; j < 12; ++j) {
        possibleIntFields.push(String.fromCharCode(97 + j));
    }
    for (let j = 0; j < 3; ++j) {
        possibleObjFields.push(String.fromCharCode(119 + j));
    }

    shuffleArray(possibleIntFields);
    shuffleArray(possibleObjFields);

    let n = 8 + Random.randInt(4);
    for (let j = 0; j < n; ++j) {
        let c = possibleIntFields[j];

        if (Random.rand() < 0.70) {
            projectionDoc[c] = 1;
            if (fields.hasOwnProperty(c)) {
                newFields[c] = true;
            }
        } else if (Random.rand() < 0.85) {
            let d = String.fromCharCode(97 + Random.randInt(12));

            projectionDoc[c] = "$" + d;
            if (fields.hasOwnProperty(d)) {
                newFields[c] = true;
            }
        } else {
            let num = Random.randInt(100);

            projectionDoc[c] = {"$literal": num};
            newFields[c] = true;
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

        if (fields.hasOwnProperty(c)) {
            newFields[c] = true;
        }
    }

    let newPipeline = pipeline.concat([{$project: projectionDoc}]);

    return {pipeline: newPipeline, fields: newFields};
}

function addExclusion(testcase, allowDottedPaths) {
    let pipeline = testcase.pipeline;
    let fields = testcase.fields;

    let projectionDoc = {};

    let possibleIntFields = [];
    let possibleObjFields = [];

    for (let j = 0; j < 12; ++j) {
        possibleIntFields.push(String.fromCharCode(97 + j));
    }
    for (let j = 0; j < 3; ++j) {
        possibleObjFields.push(String.fromCharCode(119 + j));
    }

    shuffleArray(possibleIntFields);
    shuffleArray(possibleObjFields);

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

    let newPipeline = pipeline.concat([{$project: projectionDoc}]);

    let newFields = {};
    for (let c in fields) {
        if (!projectionDoc.hasOwnProperty(c)) {
            newFields[c] = true;
        }
    }

    return {pipeline: newPipeline, fields: newFields};
}

function addAddFields(testcase, allowDottedPaths = true) {
    let pipeline = testcase.pipeline;
    let fields = testcase.fields;

    let addFieldsDoc = {};
    let newFields = {};
    for (let c in fields) {
        newFields[c] = true;
    }

    let possibleIntFields = [];
    let possibleObjFields = [];

    for (let j = 0; j < 12; ++j) {
        possibleIntFields.push(String.fromCharCode(97 + j));
    }
    for (let j = 0; j < 3; ++j) {
        possibleObjFields.push(String.fromCharCode(119 + j));
    }

    shuffleArray(possibleIntFields);
    shuffleArray(possibleObjFields);

    let n = 1 + Random.randInt(3);
    for (let j = 0; j < n; ++j) {
        let c = possibleIntFields[j];

        if (Random.rand() < 0.65) {
            let num = Random.randInt(100);

            addFieldsDoc[c] = num;
            newFields[c] = true;
        } else {
            let d = String.fromCharCode(97 + Random.randInt(20));

            addFieldsDoc[c] = "$" + d;
            if (fields.hasOwnProperty(d)) {
                newFields[c] = true;
            }
        }
    }

    n = Random.randInt(2);
    for (let j = 0; j < n; ++j) {
        let c = possibleObjFields[j];

        if (Random.rand() < 0.5 || !allowDottedPaths) {
            let d = String.fromCharCode(119 + Random.randInt(3));

            addFieldsDoc[c] = "$" + d;
            if (fields.hasOwnProperty(d)) {
                newFields[c] = true;
            }
        } else {
            let possibleSubfields = ["a", "b", "c"];
            shuffleArray(possibleSubfields);

            let m = 1 + Random.randInt(2);
            for (let i = 0; i < m; ++i) {
                let path = c + "." + possibleSubfields[i];
                let num = Random.randInt(100);

                addFieldsDoc[path] = num;
            }

            newFields[c] = true;
        }
    }

    let newPipeline = pipeline.concat([{$addFields: addFieldsDoc}]);

    return {pipeline: newPipeline, fields: newFields};
}

function addMatch(testcase, allowDottedPaths = true) {
    let pipeline = testcase.pipeline;
    let fields = testcase.fields;

    let matchDoc = {};
    let newFields = {};
    for (let c in fields) {
        newFields[c] = true;
    }

    let possibleIntFields = [];
    let possibleObjFields = [];

    for (let c in fields) {
        if (c < "w") {
            possibleIntFields.push(c);
        } else {
            possibleObjFields.push(c);
        }
    }

    if (possibleIntFields.length == 0) {
        possibleIntFields.push(String.fromCharCode(97 + Random.randInt(12)));
    }

    if (possibleObjFields.length == 0) {
        possibleObjFields.push(String.fromCharCode(119 + Random.randInt(3)));
    }

    shuffleArray(possibleIntFields);
    shuffleArray(possibleObjFields);

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

    let newPipeline = pipeline.concat([{$match: matchDoc}]);

    return {pipeline: newPipeline, fields: newFields};
}

function addGroup(testcase) {
    let pipeline = testcase.pipeline;
    let fields = testcase.fields;

    let groupDoc = {};
    let newFields = {};

    let possibleIntFields = [];
    let possibleObjFields = [];

    let fieldsArray = Object.keys(fields);
    shuffleArray(fieldsArray);

    let g = fieldsArray.length > 0 ? fieldsArray[0] : String.fromCharCode(97 + Random.randInt(12));
    groupDoc["_id"] = "$" + g;

    for (let j = 0; j < 12; ++j) {
        possibleIntFields.push(String.fromCharCode(97 + j));
    }
    for (let j = 0; j < 3; ++j) {
        possibleObjFields.push(String.fromCharCode(119 + j));
    }

    shuffleArray(possibleIntFields);
    shuffleArray(possibleObjFields);

    let n = 8 + Random.randInt(4);
    for (let j = 0; j < n; ++j) {
        let c = possibleIntFields[j];
        let sumArg = "$" + c;

        groupDoc[c] = {$sum: sumArg};
        if (fields.hasOwnProperty(c)) {
            newFields[c] = true;
        }
    }

    for (let j = 0; j < 3; ++j) {
        let c = possibleObjFields[j];
        let minArg = "$" + c;

        groupDoc[c] = {$min: minArg};
        if (fields.hasOwnProperty(c)) {
            newFields[c] = true;
        }
    }

    let newPipeline = pipeline.concat([{$group: groupDoc}]);

    return {pipeline: newPipeline, fields: newFields};
}

function addLookupUnwind(testcase) {
    let pipeline = testcase.pipeline;
    let fields = testcase.fields;

    let lookupDoc = {from: foreignCollName, localField: "_id", foreignField: "_id"};
    let newFields = {};
    for (let c in fields) {
        newFields[c] = true;
    }

    let c = String.fromCharCode(119 + Random.randInt(3));
    lookupDoc["as"] = c;

    let unwindArg = "$" + c;

    let newPipeline = pipeline.concat([{$lookup: lookupDoc}, {$unwind: unwindArg}]);

    return {pipeline: newPipeline, fields: newFields};
}

function addSort(testcase, addLimit = false) {
    let pipeline = testcase.pipeline;
    let fields = testcase.fields;

    let sortDoc = {};
    let newFields = {};
    for (let c in fields) {
        newFields[c] = true;
    }

    let possibleIntFields = [];
    let possibleObjFields = [];

    for (let j = 0; j < 12; ++j) {
        possibleIntFields.push(String.fromCharCode(97 + j));
    }
    for (let j = 0; j < 3; ++j) {
        possibleObjFields.push(String.fromCharCode(119 + j));
    }

    shuffleArray(possibleIntFields);
    shuffleArray(possibleObjFields);

    let n = 1 + Random.randInt(2);
    for (let j = 0; j < n; ++j) {
        let c = possibleIntFields[j];
        sortDoc[c] = Random.randInt(2) == 0 ? 1 : -1;
    }

    let newPipeline = pipeline.concat([{$sort: sortDoc}]);

    if (addLimit) {
        let num = 5 + Random.randInt(6);
        newPipeline = newPipeline.concat([{$limit: num}]);
    }

    return {pipeline: newPipeline, fields: newFields};
}

function generateTestcase(
    {allowInclusion, allowGroup, allowLookup, allowSort, allowSortWithLimit, allowDottedPaths}) {
    // Initialize 'testcase'.
    let testcase = {pipeline: [], fields: []};
    for (let j = 0; j < 11; ++j) {
        let c = String.fromCharCode((j < 9) ? 97 + j : 119 + j - 16);
        testcase.fields[c] = true;
    }

    let numStages = Random.randInt(9) + 4;

    for (let i = 0; i < numStages;) {
        let r = Random.rand();
        let updatedTestcase = null;

        if (r < 0.10) {
            if (allowInclusion !== true) {
                continue;
            }
            updatedTestcase = addInclusion(testcase, allowDottedPaths === true);
        } else if (r < 0.30) {
            updatedTestcase = addExclusion(testcase, allowDottedPaths === true);
        } else if (r < 0.55) {
            updatedTestcase = addAddFields(testcase, allowDottedPaths === true);
        } else if (r < 0.70) {
            updatedTestcase = addMatch(testcase, allowDottedPaths === true);
        } else if (r < 0.80) {
            if (allowGroup !== true) {
                continue;
            }

            updatedTestcase = addGroup(testcase);
        } else if (r < 0.90) {
            if (allowLookup !== true) {
                continue;
            }

            updatedTestcase = addLookupUnwind(testcase);
        } else {
            if (allowSort !== true) {
                continue;
            }

            if (r < 0.95 || allowSortWithLimit !== true) {
                updatedTestcase = addSort(testcase);
            } else {
                updatedTestcase = addSort(testcase, true);
            }
        }

        if (Object.keys(updatedTestcase.fields).length != 0) {
            testcase = updatedTestcase;
        }

        ++i;
    }

    return testcase;
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

            let testcase = generateTestcase({
                allowInclusion,
                allowGroup,
                allowLookup,
                allowSort,
                allowSortWithLimit,
                allowDottedPaths
            });

            testcases.push({id: testcaseId, pipeline: testcase.pipeline});
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
