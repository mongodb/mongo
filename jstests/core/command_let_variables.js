// Tests that commands like find, aggregate and update accepts a 'let' parameter which defines
// variables for use in expressions within the command.
// @tags: [assumes_against_mongod_not_mongos]

(function() {
"use strict";

const coll = db.update_let_variables;
coll.drop();

assert.commandWorked(coll.insert([
    {
        Species: "Blackbird (Turdus merula)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -16, annual: -0.38, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: -2, annual: -0.36, trend: "no change"}
        ]
    },
    {
        Species: "Bullfinch (Pyrrhula pyrrhula)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -39, annual: -1.13, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: 12, annual: 2.38, trend: "weak increase"}
        ]
    },
    {
        Species: "Chaffinch (Fringilla coelebs)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: 27, annual: 0.55, trend: "no change"},
            {term: {start: 2009, end: 2014}, pct_change: -7, annual: -1.49, trend: "weak decline"}
        ]
    },
    {
        Species: "Song Thrush (Turdus philomelos)",
        population_trends: [
            {term: {start: 1970, end: 2014}, pct_change: -53, annual: -1.7, trend: "weak decline"},
            {term: {start: 2009, end: 2014}, pct_change: -4, annual: -0.88, trend: "no change"}
        ]
    }
]));

// Update
assert.commandWorked(db.runCommand({
    update: coll.getName(),
    let : {target_species: "Song Thrush (Turdus philomelos)", new_name: "Song Thrush"},
    updates: [
        {q: {$expr: {$eq: ["$Species", "$$target_species"]}}, u: [{$set: {Species: "$$new_name"}}]}
    ]
}));

assert.commandWorked(db.runCommand({
    update: coll.getName(),
    let : {target_species: "Song Thrush (Turdus philomelos)"},
    updates: [{
        q: {$expr: {$eq: ["$Species", "$$target_species"]}},
        u: [{$set: {Location: "$$place"}}],
        c: {place: "North America"}
    }]
}));
}());