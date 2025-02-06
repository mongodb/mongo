/*
  Datasets with different distributions, in particular skewed ones.
  The test creates histograms over those with a constrained number of buckets.
  This serves to verify that the skewed-ness and/or anomalies within each
  dataset are properly captured even by an imperfect histogram
*/

export class UniformDataset {
    docs() {
        let uniform_docs = [];
        for (let i = 0; i < 1000; i++) {
            uniform_docs.push({a: i});
        }
        return uniform_docs;
    }

    predicates() {
        return [
            {a: 50},
            {a: {$lt: 25}},
            {a: {$lte: 25}},

            {a: {$gt: 75}},
            {a: {$gte: 75}},

            {$and: [{a: {$gt: 49}}, {a: {$lt: 51}}]},
            {$and: [{a: {$gte: 50}}, {a: {$lte: 50}}]},
        ];
    }
}

export class OnePeakDataset {
    // Peak at {a: 50}
    docs() {
        let one_peak_docs = [];
        for (let i = 0; i < 1000; i++) {
            one_peak_docs.push({a: 50});
            one_peak_docs.push({a: i});
        }
        return one_peak_docs;
    }

    predicates() {
        return [
            {a: 50},
            {a: 49},
            {a: 51},
            {a: {$gt: 50}},
            {a: {$lte: 50}},
        ];
    }
}

export class OneHoleDataset {
    // Hole at {a:1}
    docs() {
        let one_hole_docs = [];
        for (let i = 0; i < 1000; i++) {
            one_hole_docs.push({a: 0});
            one_hole_docs.push({a: 2});
        }
        return one_hole_docs;
    }

    predicates() {
        return [
            {a: -1},
            {a: 0},
            {a: 1},
            {a: 2},
            {a: 3},
            {a: {$ne: 1}},
            {$and: [{a: {$gte: 1}}, {a: {$lte: 1}}]},
            {$and: [{a: {$gt: 0}}, {a: {$lt: 2}}]},
            {$and: [{a: {$gte: 0}}, {a: {$lte: 2}}]},
        ];
    }
}

export class ThreePeakDataset {
    docs() {
        let three_peak_docs = [];
        for (let i = 0; i < 1000; i++) {
            three_peak_docs.push({a: i});
            three_peak_docs.push({a: 20});
            three_peak_docs.push({a: 50});
            three_peak_docs.push({a: 75});
        }
        return three_peak_docs;
    }

    predicates() {
        let predicates = [];

        // -1, 0, +1 for each peak
        for (const peak of [20, 50, 75]) {
            for (const offset of [-1, 0, +1]) {
                predicates.push({a: peak + offset});
                predicates.push({a: {$gt: peak + offset}});
                predicates.push({a: {$lte: peak + offset}});
            }
        }

        // And some more complex expressions for completeness
        predicates.push({$and: [{a: {$gte: 25}}, {a: {$lte: 75}}]});
        predicates.push({$and: [{a: {$gt: 25}}, {a: {$lt: 75}}]});

        return predicates;
    }
}

export class SkewedDataset {
    // A skewed distribution with one very frequent and one very infrequent value

    docs() {
        const skewed_docs = [];

        // Very infrequent value
        skewed_docs.push({a: -2});

        // Very frequent value
        for (let i = 0; i < 1000; i++) {
            skewed_docs.push({a: -1});
        }

        // Filler
        for (let i = 0; i < 1000; i++) {
            skewed_docs.push({a: i});
            skewed_docs.push({a: i % 2});
            skewed_docs.push({a: i % 3});
            skewed_docs.push({a: i % 5});
        }
        return skewed_docs;
    }

    predicates() {
        const skewed_predicates = [];
        for (let val of [-2, -1, 0]) {
            skewed_predicates.push({a: val});
            skewed_predicates.push({a: {$ne: val}});
            skewed_predicates.push({a: {$gt: val}});
            skewed_predicates.push({a: {$gte: val}});
            skewed_predicates.push({a: {$lt: val}});
            skewed_predicates.push({a: {$lte: val}});
        }

        return skewed_predicates;
    }
}
