/*
  A dataset with an array column and a set of relevant predicates.
*/

export class ArrayDataset {
    docs() {
        let array_docs = [];
        let array = [];
        for (let i = 0; i < 100; i++) {
            array_docs.push({a: array.slice()});
            array.push(i);
        }
        return array_docs;
    }

    predicates() {
        return [
            // TODO(SERVER-99630): {a: null},
            {a: -1},
            {a: 50},
            // TODO(SERVER-99634): {a: {$all:[]}},
            {a: {$all: [-1]}},
            {a: {$all: [-1, 50]}},
            // TODO(SERVER-98085): {a: {$all:[50,75]}},
            // Not estimated via histograms: {a: {$size: 50}},
            // TODO(SERVER-99025): {a: {$gt: 900}},
            // TODO(SERVER-99025): {a: {$gt: 250, $lt: 750}},

            /* TODO(SERVER-100451): Not supported under histogramCE:
            {a: {$elemMatch: {$eq: 50}}},
            {a: {$elemMatch: {$gt: 50}}},
            {a: {$elemMatch: {$ne: 50}}}
            */
        ];
    }
}
