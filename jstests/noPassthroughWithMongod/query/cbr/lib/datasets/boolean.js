/*
  A dataset with a boolean column and relevant predicates for it
*/

export class BooleanDataset {
    docs() {
        let boolean_docs = [];

        for (let i = 0; i < 100; i++) {
            boolean_docs.push({a: 0});
            boolean_docs.push({a: 1});
            boolean_docs.push({a: 1.0});
            boolean_docs.push({a: ""});
            boolean_docs.push({a: true});
            boolean_docs.push({a: false});
            boolean_docs.push({a: null});
            boolean_docs.push({b: 123});
        }

        return boolean_docs;
    }

    predicates() {
        let boolean_predicates = [];

        for (let val of [true,
                         false,
                         // TODO(SERVER-98094): null
        ]) {
            boolean_predicates.push({a: val});
            boolean_predicates.push({a: {$gt: val}});
            boolean_predicates.push({a: {$gte: val}});

            boolean_predicates.push({a: {$lt: val}});
            boolean_predicates.push({a: {$lte: val}});

            boolean_predicates.push({a: {$ne: val}});
        }
        return boolean_predicates;
    }
}
