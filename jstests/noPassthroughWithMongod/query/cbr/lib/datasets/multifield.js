/*
   A dataset with two uncorrelated fields.
*/

export class TwoFieldDataset {
    docs() {
        const two_field_docs = [];
        for (let i = 0; i < 1000; i++) {
            two_field_docs.push({a: i % 3, b: i % 5});
        }
        return two_field_docs;
    }

    predicates() {
        return [
            // With this framework, we can only estimate expressions
            // where one predicate or the other matches all or no documents.
            // If both predicates match a subset of document, the estimate
            // is no longer completely accurate so this case can not be included
            // here.

            // One predicate matches all the documents
            {a: {$gte: 0}, b: 4},
            {a: 2, b: {$lt: 10}},

            // No rows matching
            {a: {$lt: 0}, b: 4},
            {a: 2, b: {$lt: 0}},
            {a: 2, b: -1},
            {a: -1, b: 2},
            {a: -1, b: -1},
        ];
    }
}
