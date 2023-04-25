/**
 * A test for conjunctive and disjunctive predicates using a semi-realistic collection/queries.
 * @tags: [
 *   requires_cqf,
 * ]
 */

(function() {
load("jstests/libs/ce_stats_utils.js");  // For 'getRootCE', 'createHistogram'.

const collCard = 300;
const numberBuckets = 5;

const coll = db.ce_mixed;
coll.drop();

function generateData(num) {
    const firstNames =
        ["Elizabeth", "William", "John", "Jane", "Kate", "Tom", "Tim", "Bob", "Alice"];
    const lastNames = ["Smith", "Green", "Parker", "Bennet", "Knight", "Darcy", "Watson", "Holmes"];
    const allToppings =
        ["mushrooms", "prosciutto", "ham", "pepperoni", "bell peppers", "anchovies", "pineapple"];
    const pizzaShops = ["Pacinos", "Zizzi", "Milanos", "PI"];

    function getName(i) {
        return `${firstNames[i % firstNames.length]} ${lastNames[i % lastNames.length]}`;
    }

    function getDate(i) {
        const padZero = (x) => x >= 10 ? x : `0${x}`;
        const yy = 1950 + i % 50;
        const mm = padZero((i % 12) + 1);
        const dd = padZero((i % 28) + 1);
        return new ISODate(`${yy}-${mm}-${dd}T00:00:00`);
    }

    function getPizzaToppings(i) {
        let toppings = [];
        for (let j = 0; j < (i % 5); j++) {
            toppings.push(allToppings[j % allToppings.length]);
        }
        return toppings;
    }

    function getPizzaShop(i) {
        return i > 50 ? pizzaShops[i % pizzaShops.length] : pizzaShops[0];
    }

    let data = [];
    for (let i = 0; i < num; i++) {
        const likesPizza = i % 2 ? true : false;
        data.push({
            // Fields without histograms.
            _id: i,
            lastPizzaShopVisited: getPizzaShop(i),
            // Fields with histograms.
            name: getName(i),
            date: getDate(i),
            likesPizza,
            favPizzaToppings: likesPizza ? getPizzaToppings(i) : [],
        });
    }
    return data;
}

const data = generateData(collCard);
print(tojson(data));
assert.commandWorked(coll.insert(data));
assert.commandWorked(
    coll.createIndexes([{name: 1}, {date: 1}, {likesPizza: 1}, {pizzaToppings: 1}]));

function testCEForStrategy(predicate, strategy) {
    forceCE(strategy);
    const explain = coll.explain("executionStats").aggregate({$match: predicate});
    const nReturned = explain.executionStats.nReturned;
    const ce = getRootCE(explain);
    return [nReturned, ce, explain];
}

function testPredicate(predicate) {
    const [heuN, heuCE, heuExplain] = testCEForStrategy(predicate, "heuristic");
    const [hisN, hisCE, hisExplain] = testCEForStrategy(predicate, "histogram");
    assert.eq(heuN, hisN);

    jsTestLog(`Query: ${tojsononeline(predicate)} returned ${heuN} documents.`);

    if (Math.abs(heuCE - hisCE) < 0.001) {
        print(`Histogram and heuristic estimates were equal: ${heuCE}.\n`);
        print("\nHeuristic explain: ", tojson(summarizeExplainForCE(heuExplain)));
    } else {
        print(`Histogram estimate: ${hisCE}.\n`);
        print(`Heuristic estimate: ${heuCE}.\n`);
        print("Histogram explain: ", tojson(summarizeExplainForCE(hisExplain)));
    }

    print("\nHeuristic explain: ", tojson(summarizeExplainForCE(heuExplain)));
}

runHistogramsTest(function() {
    createHistogram(coll, "name", {numberBuckets});
    createHistogram(coll, "date", {numberBuckets});
    createHistogram(coll, "likesPizza", {numberBuckets});
    createHistogram(coll, "favPizzaToppings", {numberBuckets});

    // Test single predicates that use histograms.
    testPredicate({likesPizza: true});
    testPredicate({likesPizza: false});
    testPredicate({date: {$gt: new ISODate("1950-01-01T00:00:00")}});
    testPredicate({date: {$lt: new ISODate("1979-12-06T00:00:00")}});
    testPredicate({name: {$lte: "Bob Bennet"}});
    testPredicate({favPizzaToppings: "mushrooms"});

    // Test single predicates that use heuristics.
    testPredicate({lastPizzaShopVisited: "Zizzi"});
    testPredicate({lastPizzaShopVisited: "Pacinos"});

    // Test conjunctions of predicates all using histograms.
    testPredicate({
        likesPizza: true,
        date: {$gt: new ISODate("1950-01-01T00:00:00"), $lt: new ISODate("1979-12-06T00:00:00")}
    });
    testPredicate({likesPizza: false, name: {$lte: "Bob Bennet"}});
    testPredicate({favPizzaToppings: "mushrooms", name: {$lte: "Bob Bennet"}});

    // Test disjunctions of predicates all using histograms.
    testPredicate({$or: [{likesPizza: true}, {date: {$lt: new ISODate("1955-01-01T00:00:00")}}]});
    testPredicate({
        $or: [{favPizzaToppings: "mushrooms"}, {name: {$lte: "Bob Bennet", $gte: "Alice Smith"}}]
    });
    testPredicate({
        $or: [
            {$and: [{likesPizza: false}, {name: {$lte: "Bob Bennet"}}]},
            {$and: [{likesPizza: true}, {name: {$gte: "Tom Watson"}}]}
        ]
    });
    testPredicate({
        $or: [
            {$and: [{likesPizza: false}, {name: {$lte: "Bob Bennet"}}]},
            {date: {$lte: "1960-01-01T00:00:00"}}
        ]
    });

    // Test conjunctions of predicates such that some use histograms and others use heuristics.
    testPredicate({lastPizzaShopVisited: "Zizzi", likesPizza: true});
    testPredicate({lastPizzaShopVisited: "Zizzi", likesPizza: false});
    testPredicate({lastPizzaShopVisited: "Zizzi", date: {$gt: new ISODate("1950-01-01T00:00:00")}});
    testPredicate({lastPizzaShopVisited: "Zizzi", date: {$lt: new ISODate("1979-12-06T00:00:00")}});
    testPredicate({
        lastPizzaShopVisited: "Zizzi",
        date: {$gt: new ISODate("1950-01-01T00:00:00"), $lt: new ISODate("1979-12-06T00:00:00")}
    });
    testPredicate({lastPizzaShopVisited: "Pacinos", name: {$lte: "Bob Bennet"}});
    testPredicate({lastPizzaShopVisited: "Pacinos", favPizzaToppings: "mushrooms"});
    testPredicate({
        lastPizzaShopVisited: "Pacinos",
        date: {$gt: new ISODate("1950-01-01T00:00:00")},
        favPizzaToppings: "mushrooms",
        likesPizza: true
    });

    // Test disjunctions of predicates such that some use histograms and others use heuristics.
    testPredicate({$or: [{lastPizzaShopVisited: "Zizzi"}, {likesPizza: true}]});
    testPredicate({
        $or: [
            {lastPizzaShopVisited: "Zizzi"},
            {
                date: {
                    $gt: new ISODate("1950-01-01T00:00:00"),
                    $lt: new ISODate("1960-01-01T00:00:00")
                }
            }
        ]
    });
    testPredicate({
        $or: [
            {$and: [{lastPizzaShopVisited: "Zizzi"}, {name: {$lte: "John Watson"}}]},
            {$and: [{favPizzaToppings: "mushrooms"}, {likesPizza: true}]}
        ]
    });
    testPredicate({
        $or: [
            {$and: [{lastPizzaShopVisited: "Zizzi"}, {name: {$lte: "John Watson"}}]},
            {$and: [{lastPizzaShopVisited: "Zizzi"}, {name: {$gte: "Kate Knight"}}]}
        ]
    });
    testPredicate({
        $or: [
            {$and: [{lastPizzaShopVisited: "Zizzi"}, {name: {$lte: "John Watson"}}]},
            {favPizzaToppings: "mushrooms"}
        ]
    });
    testPredicate({
        $or: [
            {$and: [{favPizzaToppings: "mushrooms"}, {name: {$lte: "John Watson"}}]},
            {lastPizzaShopVisited: "Zizzi"}
        ]
    });
});
})();
