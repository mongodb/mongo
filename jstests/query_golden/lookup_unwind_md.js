/**
 * Tests combinations of $lookup and $unwind.
 */

import {normalizeArray} from "jstests/libs/golden_test.js";
import {code, linebreak, section, subSection} from "jstests/libs/query/pretty_md.js";

const lookups = [{$lookup: {from: "cities", localField: "_id", foreignField: "countryId", as: "cities"}}];

const unwinds = [{$unwind: "$cities"}, {$unwind: {path: "$cities", preserveNullAndEmptyArrays: true}}];

const matches = [
    null, // Indicates no $match should be performed.
    {$match: {_id: {$gt: 0}}},
    {$match: {nonExistentField: {$exists: false}}},
    {$match: {name: "USA", _id: {$gt: 0}}}, // One of these predicates should use an index, the
    // other will be applied residually.
    {$match: {name: "Canada", nonExistentField: {$exists: false}}},
    {$match: {$and: [{name: "France"}, {_id: 3}]}},
];

const countryDocs = [
    [],
    [
        {_id: 1, name: "USA"},
        {_id: 2, name: "Canada"},
        {_id: 3, name: "France"},
        {_id: 4, name: "Romania"},
    ],
];

const cityDocs = [
    [],
    [
        {_id: 10, countryId: 1, cityName: "New York"},
        {_id: 11, countryId: 1, cityName: "Los Angeles"},
        {_id: 12, countryId: 2, cityName: "Toronto"},
        {_id: 13, countryId: 3, cityName: "Paris"},
    ],
];

const indexes = [
    [], // No index
    [{collection: "cities", key: {countryId: 1}}],
    [{collection: "countries", key: {name: 1}}],
    [{collection: "countries", key: {_id: 1}}],
];

// The choice between HJ and NLJ is made based on the value of 'allowDiskUse' setting (because all
// data in these tests is small and that enables HJ as long as 'allowDiskUse' is 'true').
const aggOptions = [{allowDiskUse: true}, {allowDiskUse: false}];

function setupCollections(countries, cities, indexList) {
    db.countries.drop();
    db.cities.drop();
    db.countries.insertMany(countries);
    db.cities.insertMany(cities);
    indexList.forEach((idx) => {
        db[idx.collection].createIndex(idx.key);
    });
}

let counter = 0;
countryDocs.forEach((countries) => {
    cityDocs.forEach((cities) => {
        indexes.forEach((indexList) => {
            setupCollections(countries, cities, indexList);
            section(
                `countries: ${tojsononeline(countries)} - cities: ${tojsononeline(cities)} - Indexes: ${tojsononeline(indexList)}`,
            );
            linebreak();

            lookups.forEach((lookup) => {
                unwinds.forEach((unwind) => {
                    matches.forEach((match) => {
                        // Set up each pipeline. We use 'null' to indicate that a stage should be
                        // absent.
                        const pipeline = [match, lookup, unwind].filter((x) => x != null);
                        subSection("Pipeline");
                        code(tojson(pipeline));

                        aggOptions.forEach((options) => {
                            for (let i = 0; i < 3; ++i) {
                                subSection(`Options: ${tojsononeline(options)} - Iteration ${i}`);
                                code(normalizeArray(db.countries.aggregate(pipeline, options).toArray()));
                                ++counter;
                            }
                        });
                        linebreak();
                    });
                });
            });
        });
    });
});
jsTestLog("Ran " + counter + " queries");
