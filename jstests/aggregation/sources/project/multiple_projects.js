/**
 * Test that multiple $projects and $addFields in a pipeline produce correct results.
 */
const coll = db.multiple_projects;
coll.drop();

function runTest(doCoveredProjection, expected, pipeline) {
    if (doCoveredProjection) {
        let coveredProjection = {a: 1, b: 1, c: 1, d: 1, e: 1, f: 1, g: 1, h: 1, i: 1, j: 1};
        pipeline = [].concat([{$project: coveredProjection}], pipeline);
    }

    pipeline = pipeline.concat([{$match: {_id: {$mod: [2, 1]}}}]);

    assert.docEq(expected, coll.aggregate(pipeline).toArray()[0]);
}

function runTests(doCoveredProjection) {
    runTest(doCoveredProjection, {_id: 1, j: 10, g: 14, f: 17, c: 16, b: 11, a: 15, e: 18}, [
        {$project: {a: 0}},
        {$addFields: {b: 11, c: 12}},
        {$project: {d: 0, e: 0}},
        {$addFields: {f: 13, g: 14}},
        {$project: {a: 1, b: 1, c: 1, d: 1, e: 1, f: 1, g: 1, j: 1, k: 1}},
        {$addFields: {a: 15, c: 16}},
        {$project: {h: 0}},
        {$addFields: {f: 17}},
        {$project: {d: 0}},
        {$addFields: {e: 18}},
    ]);

    runTest(doCoveredProjection, {_id: 1, j: 10, g: 8, f: 14, c: 16, b: 11, e: 18}, [
        {$addFields: {b: 11, c: 12}},
        {$project: {d: 0, e: 0}},
        {$addFields: {f: 13, g: '$h'}},
        {$addFields: {f: 14}},
        {$project: {h: 0, i: 0}},
        {$addFields: {a: '$e', c: 16}},
        {$project: {d: 0}},
        {$addFields: {e: 18}},
    ]);

    runTest(
        doCoveredProjection, {_id: 1, j: 15, g: 7, f: 16, e: 14, d: 4, c: 8, b: 13, i: 12, a: 0}, [
            {$addFields: {a: '$b', c: 11}},
            {$sort: {a: 1}},
            {$addFields: {f: 12, g: '$g'}},
            {$project: {i: 0}},
            {$addFields: {b: 13, c: '$h'}},
            {$sort: {e: 1}},
            {$addFields: {e: 14}},
            {$sort: {h: 1}},
            {$addFields: {i: '$f', j: 15, a: {$ifNull: ['$a', 0]}}},
            {$project: {h: 0}},
            {$addFields: {f: 16}},
        ]);

    let subObj1 = {_id: 1, j: 10, i: 9, h: 8, g: 8, f: 13, c: 12, b: 11};

    runTest(doCoveredProjection, {_id: 1, j: 10, g: 8, f: subObj1, c: 16, b: 11, a: 15, e: 18}, [
        {$project: {a: 0}},
        {$addFields: {b: 11, c: 12}},
        {$project: {d: 0, e: 0}},
        {$addFields: {f: 13, g: '$h'}},
        {$addFields: {f: '$$ROOT'}},
        {$project: {h: 0, i: 0}},
        {$addFields: {a: 15, c: 16}},
        {$project: {d: 0}},
        {$addFields: {e: 18}},
    ]);

    runTest(doCoveredProjection, {_id: 1, j: 10, i: 9, h: 8, g: 7, f: 6, e: 5, d: 4, c: 3, b: 0}, [
        {$project: {a: 0}},
        {$addFields: {a: '$c'}},
        {$sort: {a: 1}},
        {$project: {a: 0}},
        {$addFields: {a: '$d', b: {$ifNull: ['$b', 0]}}},
        {$sort: {a: 1}},
        {$project: {a: 0}},
    ]);

    runTest(doCoveredProjection, {_id: 1, j: 10, i: 9, h: 8, g: 7, f: 6, e: 5, d: 4, c: 3, b: 11}, [
        {$project: {a: 0}},
        {$addFields: {a: '$c', b: 11}},
        {$sort: {a: 1}},
        {$project: {a: 0}},
        {$addFields: {a: '$$ROOT'}},
        {$sort: {a: 1}},
        {$project: {a: 0}},
    ]);

    runTest(doCoveredProjection,
            {_id: 1, i: 9, h: 8, g: 7, f: 14, d: 12, c: 11, a: 10, b: 10, j: 15, e: 16},
            [
                {$addFields: {a: '$j', j: 10}},
                {$addFields: {b: '$j', j: 11}},
                {$addFields: {c: '$j', j: 12}},
                {$addFields: {d: '$j', j: '$$REMOVE'}},
                {$addFields: {e: '$j', j: 14}},
                {$addFields: {f: '$j', j: 15, e: 16}},
            ]);

    if (!doCoveredProjection) {
        runTest(doCoveredProjection,
                {_id: 1, y: {_id: 1, j: 10, i: 9, h: 8, g: 7, f: 6, e: 5, d: 4, c: 3, a: 1}},
                [{$project: {x: '$$ROOT'}}, {$project: {y: '$x'}}]);

        runTest(doCoveredProjection,
                {_id: 1, x: {_id: 1, j: 10, i: 9, h: 8, g: 7, f: 6, e: 5, d: 4, c: 3, a: 1}, y: 7},
                [{$project: {x: '$$ROOT'}}, {$addFields: {y: '$x.g'}}]);

        runTest(doCoveredProjection,
                {_id: 1, y: {_id: 1, j: 10, i: 9, h: 8, g: 7, f: 6, e: 5, d: 4, c: 3, a: 1}},
                [{$addFields: {x: '$$ROOT'}}, {$project: {y: '$x'}}]);

        let subObj2 = {_id: 1, j: 10, i: 9, h: 8, g: 7, f: 6, e: 5, d: 4, c: 3, a: 1};

        runTest(doCoveredProjection,
                {_id: 1, j: 10, i: 9, h: 8, g: 7, f: 6, e: 5, d: 4, c: 3, a: 1, x: subObj2, y: 7},
                [{$addFields: {x: '$$ROOT'}}, {$addFields: {y: '$x.g'}}]);
    }
}

let indexSpec = {_id: 1, j: 1, i: 1, h: 1, g: 1, f: 1, e: 1, d: 1, c: 1, b: 1, a: 1};

assert.commandWorked(coll.insert({_id: 0, j: 10, i: 9, h: 8, g: 7, f: 6, e: 5, d: 4, c: 3, a: 1}));
assert.commandWorked(coll.insert({_id: 1, j: 10, i: 9, h: 8, g: 7, f: 6, e: 5, d: 4, c: 3, a: 1}));
assert.commandWorked(coll.insert({_id: 2, j: 10, i: 9, h: 8, g: 7, f: 6, e: 5, d: 4, c: 3, a: 1}));

assert.commandWorked(coll.createIndex(indexSpec));

let doCoveredProjection = false;
runTests(doCoveredProjection);

doCoveredProjection = true;
runTests(doCoveredProjection);
