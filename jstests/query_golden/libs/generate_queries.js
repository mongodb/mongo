/**
 * Helper functions for generating of queries over a collection.
 */

function makeMatchPredicate(field, boundary, compOp) {
    return {"$match": {[field]: {[compOp]: boundary}}};
}

function makeRangePredicate(field, op1, bound1, op2, bound2, isElemMatch = false) {
    if (isElemMatch) {
        return {"$match": {[field]: {"$elemMatch": {[op1]: bound1, [op2]: bound2}}}};
    }
    return {"$match": {[field]: {[op1]: bound1, [op2]: bound2}}};
}

function generateComparisons(field, boundaries) {
    let predicates = [];
    const compOps = ["$eq", "$lt", "$lte", "$gt", "$gte"];
    for (const op of compOps) {
        boundaries.forEach(function(boundary) {
            predicates.push(makeMatchPredicate(field, boundary, op));
        });
    }
    let docs = [];
    for (const pred of predicates) {
        let doc = {"pipeline": [pred]};
        docs.push(doc);
    }
    return docs;
}

// Generate range predicates with $match and $elemMatch.
// boundaries:[ [low1, high1], ...]
function generateRangePredicates(field, boundaries) {
    let predicates = [];
    const op1Option = ["$gt", "$gte", "$eq"];
    const op2Option = ["$lt", "$lte", "$eq"];
    boundaries.forEach(function(boundary) {
        assert(boundary.length == 2);
        for (let op1 of op1Option) {
            for (let op2 of op2Option) {
                if (op1 == "$eq" && op2 == "$eq") {
                    continue;
                }
                predicates.push(makeRangePredicate(field, op1, boundary[0], op2, boundary[1]));
                if (boundary[0] <= boundary[1]) {
                    predicates.push(
                        makeRangePredicate(field, op1, boundary[0], op2, boundary[1], true));
                }
            }
        }
    });

    let docs = [];
    for (let pred of predicates) {
        let doc = {"pipeline": [pred]};
        docs.push(doc);
    }
    return docs;
}

// Helper function to extract a number of values from a collection field, to be used for query
// generation.
function selectQueryValues(coll, field) {
    let values = [];
    const collSize = coll.find().itcount();
    let positions = [2, collSize / 2, collSize - 2];

    for (const pos of positions) {
        const res = coll.find({}, {"_id": 0, [field]: 1}).toArray()[pos];
        values.push(res[field]);
    }
    return values;
}
