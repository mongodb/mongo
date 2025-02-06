/*
  Datasets and relevant predicates for Date and Timestamp
*/

export class DateDataset {
    docs() {
        const date_docs = [];

        // Vary the year component of the date
        for (let i = 0; i < 1000; i++) {
            i = String(i).padStart(3, '0');
            date_docs.push({a: ISODate(`2${i}-01-01T01:01:01.001`)});
        }

        // Vary the subsecond component of the date
        for (let i = 0; i < 1000; i++) {
            i = String(i).padStart(3, '0');
            date_docs.push({a: ISODate(`2050-05-05T05:05:05.${i}`)});
        }

        date_docs.push({a: ISODate(0)});
        return date_docs;
    }

    predicates() {
        // Each corresponds to one of the two batches of dates from docs()
        const date1 = ISODate("2010-01-01T01:01:01.001");
        const date2 = ISODate("2050-05-05T05:05:05.050");
        return [
            {a: date1},
            {a: {$gt: date1}},
            {a: {$gte: date1}},
            {a: {$lt: date1}},
            {a: {$lte: date1}},
            {a: {$ne: date1}},

            {a: date2},
            {a: {$lt: date2}},
            {a: {$gt: date2}},

            {a: ISODate(0)},
            {a: {$ne: ISODate(0)}},
            {a: {$gt: ISODate(0)}},
        ];
    }
}

export class TimestampDataset {
    docs() {
        let timestamp_docs = [];

        for (let i = 0; i < 100; i++) {
            timestamp_docs.push({a: Timestamp(0, i)});
            timestamp_docs.push({a: Timestamp(i, 0)});
        }
        return timestamp_docs;
    }

    predicates() {
        return [
            {a: Timestamp(0, 50)},
            {a: {$gt: Timestamp(0, 50)}},
            {a: {$ne: Timestamp(0, 50)}},
            {a: Timestamp(50, 0)},
            {a: {$gt: Timestamp(50, 0)}},
            {a: {$ne: Timestamp(50, 0)}},
        ];
    }
}
