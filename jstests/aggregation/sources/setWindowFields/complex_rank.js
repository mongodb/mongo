// Tests some more complicated cases of ranking with $setWindowFields.
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insertMany([
    {x: 1, y: 1, tiebreakId: 1},
    {x: 2, y: 1, tiebreakId: 2},
    {x: 2, y: 2, tiebreakId: 3},
]));

assert.sameMembers(coll.aggregate([
                           {$setWindowFields: {sortBy: {x: 1}, output: {rank: {$rank: {}}}}},
                           {$project: {_id: 0, x: 1, rank: 1}}
                       ])
                       .toArray(),
                   [{x: 1, rank: 1}, {x: 2, rank: 2}, {x: 2, rank: 2}]);

assert.sameMembers(coll.aggregate([
                           {$replaceWith: {x: "$x"}},
                           {$setWindowFields: {sortBy: {x: 1}, output: {rank: {$rank: {}}}}},
                           {$project: {_id: 0, x: 1, rank: 1}}
                       ])
                       .toArray(),
                   [{x: 1, rank: 1}, {x: 2, rank: 2}, {x: 2, rank: 2}]);

let pipeline = [
    {$group: {_id: "$x", numOccurrences: {$sum: 1}}},
    {$setWindowFields: {sortBy: {numOccurrences: 1}, output: {rank: {$rank: {}}}}},
    {$project: {_id: 1, numOccurrences: 1, rank: 1}}
];
jsTestLog(coll.explain().aggregate(pipeline));
assert.sameMembers(coll.aggregate(pipeline).toArray(),
                   [{_id: 1, numOccurrences: 1, rank: 1}, {_id: 2, numOccurrences: 2, rank: 2}]);

assert.sameMembers(
    coll.aggregate([
            {$group: {_id: "$x", numOccurrences: {$sum: 1}}},
            {$setWindowFields: {sortBy: {numOccurrences: -1}, output: {rank: {$rank: {}}}}},
            {$project: {_id: 1, numOccurrences: 1, rank: 1}}
        ])
        .toArray(),
    [{_id: 1, numOccurrences: 1, rank: 2}, {_id: 2, numOccurrences: 2, rank: 1}]);

assert.sameMembers(
    coll.aggregate([
            {$group: {_id: "$x", numOccurrences: {$sum: 1}}},
            {
                $setWindowFields:
                    {sortBy: {'numOccurrences.missing': 1}, output: {rank: {$rank: {}}}}
            },
            {$project: {_id: 1, numOccurrences: 1, rank: 1}}
        ])
        .toArray(),
    [{_id: 1, numOccurrences: 1, rank: 1}, {_id: 2, numOccurrences: 2, rank: 1}]);
