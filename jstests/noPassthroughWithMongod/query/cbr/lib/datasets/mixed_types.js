/*
  Datasets that contain multiple datatypes within the same field.
*/

export class MixedTypesDataset {
    docs() {
        let mixed_docs = [];

        for (let i = 0; i < 100; i++) {
            // Numbers
            mixed_docs.push({a: i});
            mixed_docs.push({a: i.toString().repeat(2)});

            // Strings
            mixed_docs.push({a: 'x' + i.toString().repeat(2)});
            mixed_docs.push({a: i.toString().repeat(2) + 'y'});

            // Date, timestamp
            let threeDigitYear = String(i).padStart(3, '0');
            mixed_docs.push({a: ISODate(`2${threeDigitYear}-01-01T01:01:01.001`)}),
                mixed_docs.push({a: Timestamp(i, i)}),

                // Boolean
                mixed_docs.push({a: i % 2 === 0});

            // Object
            mixed_docs.push({a: {[i]: 1, a: i}});
        }

        mixed_docs.push({a: null});
        return mixed_docs;
    }

    predicates() {
        return [
            {a: 50},
            {a: {$gt: 50}},
            {a: {$lt: 50}},

            {a: "55"},
            {a: {$gt: "55"}},
            {a: {$lt: "55"}},

            {a: "x55"},
            {a: {$gt: "x55"}},
            {a: {$lt: "x55"}},

            {a: "55y"},
            {a: {$gt: "55y"}},
            {a: {$lt: "55y"}},

            {a: ISODate('2050-01-01T01:01:01.001')},
            {a: {$gt: ISODate('2050-01-01T01:01:01.001')}},
            {a: {$lt: ISODate('2050-01-01T01:01:01.001')}},

            {a: Timestamp(50, 50)},
            {a: {$gt: Timestamp(50, 50)}},
            {a: {$lt: Timestamp(50, 50)}},

            {a: true},
            {a: false},

            // TODO(SERVER-99628): {a: {$type: 'double'}},
            // TODO(SERVER-99628): {a: {$type: 'string'}},
            {a: {$type: 'object'}},
            {a: {$type: 'bool'}},
            {a: {$type: 'null'}},
            {a: {$type: 'regex'}},
            // TODO(SERVER-99628): {a: {$type: 'int'}},
            {a: {$type: 'timestamp'}},
            // TODO(SERVER-99628): {a: {$type: 'decimal'}},

            // Those should return no rows
            {$and: [{a: {$lt: 50}}, {a: {$gt: "x55"}}]},
            {$and: [{a: {$lt: 50}}, {a: {$lt: "55y"}}]},
            {$and: [{a: {$lt: true}}, {a: {$gt: ISODate('2050-01-01T01:01:01.001')}}]}
        ];
    }
}

export class MixedNumbersDataset {
    docs() {
        let number_docs = [];
        for (let i = 0; i < 100; i++) {
            number_docs.push({a: 1});
            number_docs.push({a: 1.0});
            number_docs.push({a: "1.0"});
            number_docs.push({a: 1.5});
            number_docs.push({a: NumberInt(1)});
            number_docs.push({a: NumberLong(1)});
            number_docs.push({a: NumberDecimal(1)});
            number_docs.push({a: NumberDecimal(1.5)});
        }
        return number_docs;
    }

    predicates() {
        return [
            {a: 1.0},
            {a: {$gt: 1.0}},
            {a: {$lte: 1.0}},
            {a: {$ne: 1.0}},

            {a: 1},
            {a: {$gt: 1}},
            {a: {$lte: 1}},
            {a: {$ne: 1}},

            {a: "1.0"},
            {a: {$gt: "1.0"}},
            {a: {$lte: "1.0"}},
            // TODO(SERVER-99628): {a: {$ne: "1.0"}},
            {a: 1.5},
            {a: {$in: [1.5, 2.5]}},
            {a: {$gt: 1.5}},
            {a: {$lte: 1.5}},
            {a: {$ne: 1.5}},

            {a: NumberInt(1)},
            {a: {$in: [NumberInt(1), NumberInt(2)]}},
            {a: {$gt: NumberInt(1)}},
            {a: {$lte: NumberInt(1)}},
            {a: {$ne: NumberInt(1)}},

            {a: NumberLong(1)},
            {a: {$in: [NumberLong(1), NumberLong(2)]}},
            {a: {$gt: NumberLong(1)}},
            {a: {$lte: NumberLong(1)}},
            {a: {$ne: NumberLong(1)}},

            {a: NumberDecimal(1)},
            {a: {$in: [NumberDecimal(1), NumberDecimal(2)]}},
            {a: {$gt: NumberDecimal(1)}},
            {a: {$lte: NumberDecimal(1)}},
            {a: {$ne: NumberDecimal(1)}},

            {a: NumberDecimal(1.5)},
            {a: {$in: [NumberDecimal(1.5), NumberDecimal(1.5)]}},
            {a: {$gt: NumberDecimal(1.5)}},
            {a: {$lte: NumberDecimal(1.5)}},
            {a: {$ne: NumberDecimal(1.5)}},

            // TODO(SERVER-99628): {a: {$type: 'double'}},
            // TODO(SERVER-99628): {a: {$type: 'int'}},
            // TODO(SERVER-99628): {a: {$type: 'decimal'}},
        ];
    }
}
