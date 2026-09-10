// SERVER-94839
// Underflow in window_function memory trackers.

(function() {
'use strict';

if (!db) {
    return;
}

const pipeline = [
    {
        "$setWindowFields": {
            "partitionBy": "_id",
            "sortBy": {"_id": 1},
            "output": {
                "result": {
                    "$min": {
                        "$function": {
                            "body": 'function(arg1, arg2){ return arg1; }',
                            "args": [
                                {"$objectToArray": {"str": "enhance dot-com Internal"}},
                            ],
                            "lang": 'js'
                        }
                    },
                    "window": {"documents": [9, 17]}
                }
            }
        }
    },
];

const documentList = [
    {"_id": 0, "time": ISODate("2024-07-11T07:35:24.150Z"), "tag": {"s": 2}},
    {"_id": 7, "time": ISODate("2024-07-11T09:11:14.916Z"), "tag": {"s": 0}},
    {"_id": 14, "time": ISODate("2024-07-11T11:18:35.921Z"), "tag": {"s": 1}},
    {"_id": 15, "time": ISODate("2024-07-11T14:10:00.688Z"), "tag": {"s": 1}},
    {"_id": 16, "time": ISODate("2024-07-11T16:16:01.993Z"), "tag": {"s": 0}},
    {"_id": 23, "time": ISODate("2024-07-11T17:10:08.909Z"), "tag": {"s": 2}},
    {"_id": 24, "time": ISODate("2024-07-11T17:40:08.679Z"), "tag": {"s": 0}},
    {"_id": 32, "time": ISODate("2024-07-11T19:00:52.124Z"), "tag": {"s": 1}},
    {"_id": 33, "time": ISODate("2024-07-11T19:38:50.454Z"), "tag": {"s": 1}},
    {"_id": 34, "time": ISODate("2024-07-11T21:06:02.867Z"), "tag": {"s": 2}},
    {"_id": 35, "time": ISODate("2024-07-11T23:09:53.495Z"), "tag": {"s": 2}},
    {"_id": 38, "time": ISODate("2024-07-11T23:48:16.384Z"), "tag": {"s": 1}},
    {"_id": 39, "time": ISODate("2024-07-12T00:33:05.525Z"), "tag": {"s": 0}},
    {"_id": 40, "time": ISODate("2024-07-12T02:25:56.291Z"), "tag": {"s": 0}},
    {"_id": 41, "time": ISODate("2024-07-12T03:59:19.521Z"), "tag": {"s": 2}},
    {"_id": 42, "time": ISODate("2024-07-12T05:21:59.858Z"), "tag": {"s": 2}},
    {"_id": 43, "time": ISODate("2024-07-12T07:58:08.402Z"), "tag": {"s": 2}},
    {"_id": 49, "time": ISODate("2024-07-12T10:23:40.626Z"), "tag": {"s": 2}},
    {"_id": 50, "time": ISODate("2024-07-12T12:59:11.562Z"), "tag": {"s": 1}},
    {"_id": 52, "time": ISODate("2024-07-12T13:39:22.881Z"), "tag": {"s": 2}},
    {"_id": 59, "time": ISODate("2024-07-12T14:22:56.676Z"), "tag": {"s": 0}},
    {"_id": 60, "time": ISODate("2024-07-12T15:46:21.936Z"), "tag": {}},
    {"_id": 61, "time": ISODate("2024-07-12T17:51:43.398Z"), "tag": {"s": 1}},
    {"_id": 67, "time": ISODate("2024-07-12T18:57:08.266Z"), "tag": {}},
    {"_id": 73, "time": ISODate("2024-07-12T19:39:42.416Z"), "tag": {"s": 1}},
    {"_id": 74, "time": ISODate("2024-07-12T22:15:22.336Z"), "tag": {"s": 1}},
    {"_id": 75, "time": ISODate("2024-07-12T23:21:39.015Z"), "tag": {"s": 0}},
    {"_id": 78, "time": ISODate("2024-07-13T00:12:40.680Z"), "tag": {"s": 0}},
    {"_id": 79, "time": ISODate("2024-07-13T02:27:33.605Z"), "tag": {"s": 0}},
    {"_id": 87, "time": ISODate("2024-07-13T02:36:02.418Z"), "tag": {"s": 0}}
];

const timeseriesParams = {
    timeField: 'time',
    metaField: 'tag',
    granularity: 'seconds',
};

db.dropDatabase();
assert.commandWorked(db.createCollection('test', {timeseries: timeseriesParams}));
assert.commandWorked(db.test.insert(documentList));

// Simply test that the query can be fully executed and does not trigger a tripwire assertion.
const res = db.test.aggregate(pipeline).toArray();
assert.gte(res.length, 30, tojson(res));
})();
