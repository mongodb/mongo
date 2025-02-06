/*
  A dataset containing mostly numbers of the same type
*/

export class NumberDataset {
    docs() {
        let docs = [];

        for (let i = 1; i < 1001; i++) {
            // Leave a 'hole' at position 15
            if (i != 15) {
                docs.push({a: i});
            }
        }

        // We insert one value with a very high frequency.
        // This will be used to test open vs. closed intervals
        for (let i = 0; i < 1000; i++) {
            docs.push({a: 50});
        }

        // Explicitly-null and missing values for 'a'
        docs.push({unrelated_field: 1});
        docs.push({a: null});

        // Infinity
        docs.push({a: Infinity});
        docs.push({a: Number.NEGATIVE_INFINITY});

        return docs;
    }

    predicates() {
        return [
            {a: 0},
            {a: 15},
            {a: 50},

            // $gt, $lt
            {a: {$gt: 5}},
            {a: {$lt: 5}},
            {a: {$gt: 15}},
            {a: {$lt: 15}},
            {a: {$gt: 50}},
            {a: {$lt: 50}},

            // //$gte, $lte
            {a: {$gte: 5}},
            {a: {$lte: 5}},
            {a: {$gte: 15}},
            {a: {$lte: 15}},
            {a: {$gte: 50}},
            {a: {$lte: 50}},

            // $ne
            // TODO(SERVER-99024): {a: {$ne: -1}},
            {a: {$ne: 50}},
            // TODO(SERVER-99024): {a: {$ne: 0}},
            {a: {$ne: "no_such_value"}},
            {a: {$ne: ""}},
            {a: {$ne: null}},

            // $in
            {a: {$in: []}},
            // TODO(SERVER-98094): {a: {$in: [null]}},
            {a: {$in: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]}},
            {a: {$nin: [1, 2, 3]}},
            {a: {$in: [-1]}},
            {a: {$in: [-1, 1]}},

            // Values that do not exist in the histogram
            {a: ''},
            {a: 'no_such_value'},
            {a: -1},

            // Null
            // TODO(SERVER-98094):  {a: null},

            // Exists
            // TODO(SERVER-98494): {a: {$exists: true}},
            // TODO(SERVER-98494): {a: {$exists: false}},

            // Negation with $nor
            {$nor: [{a: 50}]},
            // TODO(SERVER-99024): {$nor: [{a: -1}] },
            {$nor: [{a: null}]},
            {$nor: [{a: {$gt: 90}}]},

            // Negation with $not
            // TODO(SERVER-99628): {a: {$not: {$eq: 0}}},
            {a: {$not: {$eq: 50}}},
            {a: {$not: {$eq: null}}},
            {a: {$not: {$in: [1]}}},
            {a: {$not: {$in: [1, 2, 3]}}},
            {a: {$not: {$gt: 0}}},

            // $type

            // TODO(SERVER-99628): {a: {$type: "int"}},
            // TODO(SERVER-99628): {a: {$type: "long"}},
            {a: {$type: "null"}},
            {a: {$type: "string"}},

            // Infinity
            {a: Infinity},

            {a: {$gt: Infinity}},

            {a: {$lt: Infinity}},
            {a: Number.NEGATIVE_INFINITY},
            {a: {$gt: Number.NEGATIVE_INFINITY}},

            {a: {$lt: Number.NEGATIVE_INFINITY}},

            // Compound conditions over a single field
            {a: {$gt: 25, $lt: 50}},
            // 1 document matches
            {a: {$gte: 25, $lte: 25}},
            {a: {$gt: 24, $lt: 26}},
            // No documents matching
            {a: {$gte: 15, $lte: 15}},
            {a: {$gt: 14, $lt: 16}},

            // Empty interval
            {a: {$lt: 25, $gt: 50}},

            // $or
            {$or: [{a: 1}, {a: 10}]},
            {$or: [{a: 1}, {a: 0}]},
            {$or: [{a: 1}, {a: -1}]},
            {$or: [{a: -1}, {a: -2}]},

            {$and: [{a: {$gte: MinKey()}}, {$or: [{a: 1}, {a: {$gt: 90}}]}]},
            // TODO(SERVER-97790): {$or: [{a:1}, {a:{$gt: 90}}]},

            // $and
            // No rows match
            {$and: [{a: 1}, {a: 0}]},

            {$and: [{a: {$gt: 25}}, {a: {$gt: 50}}]},
            {$and: [{a: {$gt: 25}}, {a: {$gte: 50}}]},

            {$and: [{a: {$gt: 25}}, {a: {$lt: 50}}]},
            {$and: [{a: {$gt: 25}}, {a: {$lte: 50}}]},
            {$and: [{a: {$gt: 50}}, {a: {$lt: 75}}]},
            {$and: [{a: {$gte: 25}}, {a: {$lt: 75}}]},

            // Intervals that boil down to a single value
            // This is equivalent to $eq: -1
            {$and: [{a: {$gte: -1}}, {a: {$lte: -1}}]},
            // This is equivalent to $eq: 1
            {$and: [{a: {$gte: 1}}, {a: {$lte: 1}}]},
            // This is equivalent to $eq: 50
            {$and: [{a: {$gte: 50}}, {a: {$lte: 50}}]},

            // $regex
            // Not estimated via histograms: {a: {$regex: "^b"}}

            // Array predicate on a non-array column is unestimable
            // {a:[1]}

            // Nested $and and $or
            {$and: [{$or: [{a: 0}, {a: 1}]}, {$or: [{a: {$lt: 90}}]}]},
            {$and: [{a: {$gte: MinKey()}}, {$or: [{a: 1}, {a: {$gt: 90}}]}]}

            /* TODO(SERVER-99710) Not estimated via histograms
                {$or: [
                       {$and: [{a:{$gt: 5 }}, {a: {$lt: 20}}]},
                       {$and: [{a:{$gt: 50}}, {a: {$lt: 70}}]}
                ]}
            */
        ];
    }
}
