const kDbName = 'test_reshard';
const kCollName = 'reshard_coll';

const numDonors = 3;


const numRecipients = 1;


const reshardInPlace = true;


const enableElections = true;


const skipCheckSameResultForFindAndModify = false;


const steps = [
    { // 0
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 0,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 3},
            {_id: 1,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 18},
            {_id: 2,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 11},
            {_id: 3,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 7},
            {_id: 4,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 13},
            {_id: 5,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 11},
            {_id: 6,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 2},
            {_id: 7,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 5},
            {_id: 8,donor: 'donor0',recipient: 'recipient0',slot: 25,num: 3},
            {_id: 9,donor: 'donor2',recipient: 'recipient0',slot: 12,num: 8},
        ],
    },
    { // 1
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 10,donor: 'donor0',recipient: 'recipient0',slot: 22,num: 13},
            {_id: 11,donor: 'donor2',recipient: 'recipient0',slot: 22,num: 8},
            {_id: 12,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 4},
            {_id: 13,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 3},
            {_id: 14,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 9},
            {_id: 15,donor: 'donor0',recipient: 'recipient0',slot: 43,num: 5},
            {_id: 16,donor: 'donor2',recipient: 'recipient0',slot: 6,num: 11},
            {_id: 17,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 20},
            {_id: 18,donor: 'donor2',recipient: 'recipient0',slot: 19,num: 18},
            {_id: 19,donor: 'donor2',recipient: 'recipient0',slot: 22,num: 4},
        ],
    },
    { // 2
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 20,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 21,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 22,donor: 'donor0',recipient: 'recipient0',slot: 17,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 23,donor: 'donor0',recipient: 'recipient0',slot: 21,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 24,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 25,donor: 'donor0',recipient: 'recipient0',slot: 0,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 26,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 27,donor: 'donor1',recipient: 'recipient0',slot: 18,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 28,donor: 'donor0',recipient: 'recipient0',slot: 21,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 29,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 10}]}},
        ],
    },
    { // 3
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 30,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 19}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 26 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 0 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 31,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 18},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 27 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 25 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 32,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 14}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 0 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 33,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 18}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 23},update: {$set: { donor: 'donor0' },$inc: { dummy: 1 }},new: true}},
        ],
    },
    { // 4
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 34,donor: 'donor1',recipient: 'recipient0',slot: 35,num: 17},
            {_id: 35,donor: 'donor2',recipient: 'recipient0',slot: 38,num: 17},
            {_id: 36,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 1},
            {_id: 37,donor: 'donor2',recipient: 'recipient0',slot: 23,num: 17},
            {_id: 38,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 5},
            {_id: 39,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 12},
            {_id: 40,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 7},
            {_id: 41,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 12},
            {_id: 42,donor: 'donor0',recipient: 'recipient0',slot: 32,num: 7},
            {_id: 43,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 19},
        ],
    },
    { // 5
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 9 },u: { $set: { num: 18 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 5 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 23 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 44,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 17}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 11},u: [{ $set: { recipient: 'recipient0' } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 17},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 42},u: {donor: 'donor0',recipient: 'recipient0',slot: 15,num: 8}}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 15},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 15},update: [{ $set: { recipient: 'recipient0' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 26 },limit: 1}]}},
        ],
    },
    { // 6
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 45,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 17}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 37 },u: { $set: { num: 7 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 2},update: [{ $set: {donor: 'donor1',recipient: 'recipient0'} },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 46,donor: 'donor2',recipient: 'recipient0',slot: 29,num: 20}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 29},u: { $set: { num: 19 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 11 },u: [{ $set: { num: 20 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 24},u: { $set: {donor: 'donor0',recipient: 'recipient0'} }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 43 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',recipient: 'recipient0',slot: 22},u: {donor: 'donor1',recipient: 'recipient0',slot: 5,num: 2}}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 48},remove: true}},
        ],
    },
    { // 7
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 47,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 48,donor: 'donor1',recipient: 'recipient0',slot: 18,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 49,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 50,donor: 'donor2',recipient: 'recipient0',slot: 39,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 51,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 52,donor: 'donor2',recipient: 'recipient0',slot: 17,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 53,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 54,donor: 'donor1',recipient: 'recipient0',slot: 19,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 55,donor: 'donor1',recipient: 'recipient0',slot: 6,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 56,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 15}]}},
        ],
    },
    { // 8
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 57,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 10}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 41 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 58,donor: 'donor2',recipient: 'recipient0',slot: 45,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 59,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 19}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 27 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 29},update: [{ $set: { recipient: 'recipient0' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 5},u: [{ $set: {donor: 'donor1',recipient: 'recipient0'} }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 60,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 7}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 29 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 29},update: [{ $set: { num: 19 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
        ],
    },
    { // 9
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 61,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 62,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 63,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 64,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 65,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 66,donor: 'donor1',recipient: 'recipient0',slot: 9,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 67,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 68,donor: 'donor0',recipient: 'recipient0',slot: 19,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 69,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 70,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 8}]}},
        ],
    },
    { // 10
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 71,donor: 'donor0',recipient: 'recipient0',slot: 19,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 72,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 73,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 74,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 75,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 76,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 77,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 78,donor: 'donor1',recipient: 'recipient0',slot: 28,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 79,donor: 'donor2',recipient: 'recipient0',slot: 46,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 80,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 2}]}},
        ],
    },
    { // 11
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 81,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 82,donor: 'donor2',recipient: 'recipient0',slot: 25,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 83,donor: 'donor2',recipient: 'recipient0',slot: 13,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 84,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 85,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 86,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 87,donor: 'donor0',recipient: 'recipient0',slot: 4,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 88,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 89,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 90,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 3}]}},
        ],
    },
    { // 12
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 91,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 17},
            {_id: 92,donor: 'donor0',recipient: 'recipient0',slot: 17,num: 1},
            {_id: 93,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 18},
            {_id: 94,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 4},
            {_id: 95,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 20},
            {_id: 96,donor: 'donor0',recipient: 'recipient0',slot: 43,num: 8},
            {_id: 97,donor: 'donor2',recipient: 'recipient0',slot: 39,num: 14},
            {_id: 98,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 7},
            {_id: 99,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 3},
            {_id: 100,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 11},
        ],
    },
    { // 13
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 101,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 102,donor: 'donor0',recipient: 'recipient0',slot: 35,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 103,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 104,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 105,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 106,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 107,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 108,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 109,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 110,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 16}]}},
        ],
    },
    { // 14
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 111,donor: 'donor2',recipient: 'recipient0',slot: 47,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 112,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 113,donor: 'donor0',recipient: 'recipient0',slot: 1,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 114,donor: 'donor0',recipient: 'recipient0',slot: 17,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 115,donor: 'donor0',recipient: 'recipient0',slot: 45,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 116,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 117,donor: 'donor1',recipient: 'recipient0',slot: 8,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 118,donor: 'donor1',recipient: 'recipient0',slot: 28,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 119,donor: 'donor1',recipient: 'recipient0',slot: 5,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 120,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 1}]}},
        ],
    },
    { // 15
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 121,donor: 'donor0',recipient: 'recipient0',slot: 29,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 122,donor: 'donor1',recipient: 'recipient0',slot: 20,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 123,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 124,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 125,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 126,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 127,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 128,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 129,donor: 'donor1',recipient: 'recipient0',slot: 8,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 130,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 2}]}},
        ],
    },
    { // 16
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 131,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 132,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 133,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 134,donor: 'donor1',recipient: 'recipient0',slot: 4,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 135,donor: 'donor0',recipient: 'recipient0',slot: 4,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 136,donor: 'donor0',recipient: 'recipient0',slot: 30,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 137,donor: 'donor2',recipient: 'recipient0',slot: 12,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 138,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 139,donor: 'donor2',recipient: 'recipient0',slot: 19,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 140,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 19}]}},
        ],
    },
    { // 17
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 141,donor: 'donor0',recipient: 'recipient0',slot: 30,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 142,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 143,donor: 'donor2',recipient: 'recipient0',slot: 25,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 144,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 145,donor: 'donor0',recipient: 'recipient0',slot: 36,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 146,donor: 'donor0',recipient: 'recipient0',slot: 3,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 147,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 148,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 149,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 150,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 13}]}},
        ],
    },
    { // 18
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 151,donor: 'donor1',recipient: 'recipient0',slot: 3,num: 6},
            {_id: 152,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 2},
            {_id: 153,donor: 'donor0',recipient: 'recipient0',slot: 1,num: 2},
            {_id: 154,donor: 'donor1',recipient: 'recipient0',slot: 9,num: 17},
            {_id: 155,donor: 'donor0',recipient: 'recipient0',slot: 17,num: 14},
            {_id: 156,donor: 'donor0',recipient: 'recipient0',slot: 34,num: 6},
            {_id: 157,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 3},
            {_id: 158,donor: 'donor0',recipient: 'recipient0',slot: 28,num: 2},
            {_id: 159,donor: 'donor2',recipient: 'recipient0',slot: 8,num: 1},
            {_id: 160,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 17},
        ],
    },
    { // 19
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 161,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 162,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 163,donor: 'donor0',recipient: 'recipient0',slot: 11,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 164,donor: 'donor1',recipient: 'recipient0',slot: 9,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 165,donor: 'donor0',recipient: 'recipient0',slot: 17,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 166,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 167,donor: 'donor0',recipient: 'recipient0',slot: 25,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 168,donor: 'donor1',recipient: 'recipient0',slot: 6,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 169,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 170,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 12}]}},
        ],
    },
    { // 20
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 171,donor: 'donor1',recipient: 'recipient0',slot: 5,num: 8},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 170 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 172,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 6}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 173,donor: 'donor1',recipient: 'recipient0',slot: 4,num: 12},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 6},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 5 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 10 },u: [{ $set: { num: 20 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 28},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 174,donor: 'donor2',recipient: 'recipient0',slot: 4,num: 1},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 8},u: { $set: { recipient: 'recipient0' } }}]}},
        ],
    },
    { // 21
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 175,donor: 'donor1',recipient: 'recipient0',slot: 9,num: 17},
            {_id: 176,donor: 'donor0',recipient: 'recipient0',slot: 34,num: 1},
            {_id: 177,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 20},
            {_id: 178,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 2},
            {_id: 179,donor: 'donor2',recipient: 'recipient0',slot: 45,num: 15},
            {_id: 180,donor: 'donor2',recipient: 'recipient0',slot: 4,num: 15},
            {_id: 181,donor: 'donor0',recipient: 'recipient0',slot: 36,num: 20},
            {_id: 182,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 8},
            {_id: 183,donor: 'donor1',recipient: 'recipient0',slot: 3,num: 13},
            {_id: 184,donor: 'donor1',recipient: 'recipient0',slot: 28,num: 15},
        ],
    },
    { // 22
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 185,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 186,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 187,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 188,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 189,donor: 'donor2',recipient: 'recipient0',slot: 17,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 190,donor: 'donor0',recipient: 'recipient0',slot: 18,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 191,donor: 'donor2',recipient: 'recipient0',slot: 18,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 192,donor: 'donor1',recipient: 'recipient0',slot: 24,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 193,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 194,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 12}]}},
        ],
    },
    { // 23
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 195,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 6},
            {_id: 196,donor: 'donor1',recipient: 'recipient0',slot: 30,num: 2},
            {_id: 197,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 11},
            {_id: 198,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 6},
            {_id: 199,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 9},
            {_id: 200,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 12},
            {_id: 201,donor: 'donor0',recipient: 'recipient0',slot: 2,num: 20},
            {_id: 202,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 17},
            {_id: 203,donor: 'donor0',recipient: 'recipient0',slot: 6,num: 4},
            {_id: 204,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 16},
        ],
    },
    { // 24
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 205,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 206,donor: 'donor0',recipient: 'recipient0',slot: 25,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 207,donor: 'donor2',recipient: 'recipient0',slot: 9,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 208,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 209,donor: 'donor0',recipient: 'recipient0',slot: 41,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 210,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 211,donor: 'donor2',recipient: 'recipient0',slot: 12,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 212,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 213,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 214,donor: 'donor0',recipient: 'recipient0',slot: 28,num: 13}]}},
        ],
    },
    { // 25
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 215,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 216,donor: 'donor1',recipient: 'recipient0',slot: 18,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 217,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 218,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 219,donor: 'donor1',recipient: 'recipient0',slot: 3,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 220,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 221,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 222,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 223,donor: 'donor2',recipient: 'recipient0',slot: 14,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 224,donor: 'donor1',recipient: 'recipient0',slot: 47,num: 3}]}},
        ],
    },
    { // 26
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 113 },u: [{ $set: { num: 13 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 70 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 26},u: { $set: { donor: 'donor1' } }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 225,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 16}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 9},u: { $set: { recipient: 'recipient0' } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 226,donor: 'donor2',recipient: 'recipient0',slot: 23,num: 8},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 4},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 227,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 6}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 199 },u: { $set: { num: 13 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 164 },limit: 1}]}},
        ],
    },
    { // 27
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 228,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 229,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 230,donor: 'donor0',recipient: 'recipient0',slot: 8,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 231,donor: 'donor1',recipient: 'recipient0',slot: 6,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 232,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 233,donor: 'donor2',recipient: 'recipient0',slot: 18,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 234,donor: 'donor0',recipient: 'recipient0',slot: 45,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 235,donor: 'donor0',recipient: 'recipient0',slot: 35,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 236,donor: 'donor1',recipient: 'recipient0',slot: 32,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 237,donor: 'donor1',recipient: 'recipient0',slot: 47,num: 11}]}},
        ],
    },
    { // 28
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 23},u: [{ $set: { num: 2 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 27},u: [{ $set: { donor: 'donor0' } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 238,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 10}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 33 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 173 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 239,donor: 'donor1',recipient: 'recipient0',slot: 12,num: 20},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 240,donor: 'donor0',recipient: 'recipient0',slot: 36,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 241,donor: 'donor0',recipient: 'recipient0',slot: 17,num: 7}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 13},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 17},remove: true}},
        ],
    },
    { // 29
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 242,donor: 'donor2',recipient: 'recipient0',slot: 32,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 243,donor: 'donor0',recipient: 'recipient0',slot: 38,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 244,donor: 'donor2',recipient: 'recipient0',slot: 46,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 245,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 246,donor: 'donor0',recipient: 'recipient0',slot: 25,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 247,donor: 'donor2',recipient: 'recipient0',slot: 22,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 248,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 249,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 250,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 251,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 18}]}},
        ],
    },
    { // 30
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 0 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 164 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 33 },u: { $set: { num: 2 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 27},update: [{ $set: {donor: 'donor2',recipient: 'recipient0'} },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 252,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 7}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 60 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 10},u: {donor: 'donor2',recipient: 'recipient0',slot: 24,num: 12}}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 184 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 42 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 19 },limit: 1}]}},
        ],
    },
    { // 31
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 253,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 2},
            {_id: 254,donor: 'donor2',recipient: 'recipient0',slot: 21,num: 6},
            {_id: 255,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 2},
            {_id: 256,donor: 'donor2',recipient: 'recipient0',slot: 47,num: 9},
            {_id: 257,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 17},
            {_id: 258,donor: 'donor2',recipient: 'recipient0',slot: 25,num: 16},
            {_id: 259,donor: 'donor0',recipient: 'recipient0',slot: 34,num: 10},
            {_id: 260,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 14},
            {_id: 261,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 13},
            {_id: 262,donor: 'donor2',recipient: 'recipient0',slot: 47,num: 12},
        ],
    },
    { // 32
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 263,donor: 'donor2',recipient: 'recipient0',slot: 17,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 264,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 265,donor: 'donor0',recipient: 'recipient0',slot: 45,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 266,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 267,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 268,donor: 'donor2',recipient: 'recipient0',slot: 29,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 269,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 270,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 271,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 272,donor: 'donor0',recipient: 'recipient0',slot: 22,num: 7}]}},
        ],
    },
    { // 33
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 273,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 274,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 10}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 37},u: [{ $set: {donor: 'donor2',recipient: 'recipient0'} }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 2},update: {$set: {donor: 'donor1',recipient: 'recipient0'},$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 93 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 275,donor: 'donor2',recipient: 'recipient0',slot: 47,num: 8}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 37},u: {donor: 'donor0',recipient: 'recipient0',slot: 2,num: 15}}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 276,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 5}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 277,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 8},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 7},remove: true}},
        ],
    },
    { // 34
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 266 },u: { $set: { num: 1 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 25},u: { $set: { num: 15 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 278,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 10},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 203 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 86 },u: { $set: { num: 9 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 21},u: [{ $set: {donor: 'donor0',recipient: 'recipient0'} }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 43},update: {$set: { donor: 'donor2' },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 15},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 279,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 280,donor: 'donor0',recipient: 'recipient0',slot: 18,num: 1}]}},
        ],
    },
    { // 35
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 281,donor: 'donor0',recipient: 'recipient0',slot: 36,num: 11},
            {_id: 282,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 5},
            {_id: 283,donor: 'donor0',recipient: 'recipient0',slot: 12,num: 7},
            {_id: 284,donor: 'donor1',recipient: 'recipient0',slot: 5,num: 18},
            {_id: 285,donor: 'donor2',recipient: 'recipient0',slot: 21,num: 20},
            {_id: 286,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 19},
            {_id: 287,donor: 'donor1',recipient: 'recipient0',slot: 23,num: 2},
            {_id: 288,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 13},
            {_id: 289,donor: 'donor2',recipient: 'recipient0',slot: 22,num: 12},
            {_id: 290,donor: 'donor2',recipient: 'recipient0',slot: 39,num: 20},
        ],
    },
    { // 36
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 291,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 5},
            {_id: 292,donor: 'donor0',recipient: 'recipient0',slot: 11,num: 19},
            {_id: 293,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 1},
            {_id: 294,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 19},
            {_id: 295,donor: 'donor0',recipient: 'recipient0',slot: 22,num: 2},
            {_id: 296,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 11},
            {_id: 297,donor: 'donor2',recipient: 'recipient0',slot: 21,num: 8},
            {_id: 298,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 3},
            {_id: 299,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 18},
            {_id: 300,donor: 'donor1',recipient: 'recipient0',slot: 44,num: 2},
        ],
    },
    { // 37
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 18},u: {donor: 'donor0',recipient: 'recipient0',slot: 26,num: 18}}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 34},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 28 },u: [{ $set: { num: 11 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 48},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 138 },u: { $set: { num: 13 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 242 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 301,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 11}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 21},update: [{ $set: { num: 12 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 302,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 2}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 62 },limit: 1}]}},
        ],
    },
    { // 38
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 303,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 5 },u: { $set: { num: 1 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 89 },u: { $set: { num: 6 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 13},update: [{ $set: { num: 11 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 304,donor: 'donor0',recipient: 'recipient0',slot: 18,num: 4}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 155 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 305,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 3}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 161 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 112 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 262 },limit: 1}]}},
        ],
    },
    { // 39
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 306,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 307,donor: 'donor1',recipient: 'recipient0',slot: 24,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 308,donor: 'donor2',recipient: 'recipient0',slot: 4,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 309,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 310,donor: 'donor0',recipient: 'recipient0',slot: 18,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 311,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 312,donor: 'donor1',recipient: 'recipient0',slot: 32,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 313,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 314,donor: 'donor0',recipient: 'recipient0',slot: 36,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 315,donor: 'donor0',recipient: 'recipient0',slot: 34,num: 20}]}},
        ],
    },
    { // 40
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 316,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 19},
            {_id: 317,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 20},
            {_id: 318,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 3},
            {_id: 319,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 4},
            {_id: 320,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 5},
            {_id: 321,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 9},
            {_id: 322,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 17},
            {_id: 323,donor: 'donor0',recipient: 'recipient0',slot: 32,num: 19},
            {_id: 324,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 5},
            {_id: 325,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 7},
        ],
    },
    { // 41
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 240 },u: [{ $set: { num: 7 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 81 },u: { $set: { num: 7 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 281 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 326,donor: 'donor1',recipient: 'recipient0',slot: 44,num: 20}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 14},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 141 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 9},u: {donor: 'donor0',recipient: 'recipient0',slot: 2,num: 1}}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 43},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 157 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 327,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 3}]}},
        ],
    },
    { // 42
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 328,donor: 'donor2',recipient: 'recipient0',slot: 23,num: 4},
            {_id: 329,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 13},
            {_id: 330,donor: 'donor1',recipient: 'recipient0',slot: 30,num: 6},
            {_id: 331,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 13},
            {_id: 332,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 17},
            {_id: 333,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 11},
            {_id: 334,donor: 'donor0',recipient: 'recipient0',slot: 37,num: 20},
            {_id: 335,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 15},
            {_id: 336,donor: 'donor1',recipient: 'recipient0',slot: 25,num: 3},
            {_id: 337,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 20},
        ],
    },
    { // 43
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 338,donor: 'donor1',recipient: 'recipient0',slot: 12,num: 18}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 16},update: {$set: { num: 20 },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 339,donor: 'donor2',recipient: 'recipient0',slot: 31,num: 17},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 340,donor: 'donor0',recipient: 'recipient0',slot: 4,num: 20}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 341,donor: 'donor2',recipient: 'recipient0',slot: 2,num: 14},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 106 },u: { $set: { num: 16 } }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 342,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 343,donor: 'donor2',recipient: 'recipient0',slot: 45,num: 20}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 5},u: [{ $set: { num: 16 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 268 },limit: 1}]}},
        ],
    },
    { // 44
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 344,donor: 'donor0',recipient: 'recipient0',slot: 41,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 345,donor: 'donor2',recipient: 'recipient0',slot: 29,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 346,donor: 'donor1',recipient: 'recipient0',slot: 25,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 347,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 348,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 349,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 350,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 351,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 352,donor: 'donor1',recipient: 'recipient0',slot: 18,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 353,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 4}]}},
        ],
    },
    { // 45
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 354,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 7}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 182 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 48},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 351 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 56 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 30},update: [{ $set: { recipient: 'recipient0' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 349 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 337 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 40},update: {$set: {donor: 'donor0',recipient: 'recipient0'},$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 115 },u: { $set: { num: 11 } }}]}},
        ],
    },
    { // 46
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 355,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 356,donor: 'donor0',recipient: 'recipient0',slot: 32,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 357,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 358,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 359,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 360,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 361,donor: 'donor2',recipient: 'recipient0',slot: 13,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 362,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 363,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 364,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 9}]}},
        ],
    },
    { // 47
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 251 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 13},u: { $set: {donor: 'donor0',recipient: 'recipient0'} }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 23},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 208 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 365,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 6}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 366,donor: 'donor0',recipient: 'recipient0',slot: 29,num: 4},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 67 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 3 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 367,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 2},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 368,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 14}]}},
        ],
    },
    { // 48
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 369,donor: 'donor1',recipient: 'recipient0',slot: 16,num: 15},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 20 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 34},update: {$set: { donor: 'donor0' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 34},u: { $set: { donor: 'donor1' } }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 370,donor: 'donor0',recipient: 'recipient0',slot: 14,num: 10}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 41},update: {$set: { num: 12 },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 48 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 101 },u: [{ $set: { num: 15 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 19},update: [{ $set: {donor: 'donor2',recipient: 'recipient0'} },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 0},u: { $set: { recipient: 'recipient0' } }}]}},
        ],
    },
    { // 49
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 236 },u: [{ $set: { num: 4 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 298 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 371,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 18},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 19},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 32 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 372,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 373,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 6}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 366 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 294 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 374,donor: 'donor0',recipient: 'recipient0',slot: 6,num: 17},update: { $inc: { num: 0 } },new: true,upsert: true}},
        ],
    },
    { // 50
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 375,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 10},
            {_id: 376,donor: 'donor0',recipient: 'recipient0',slot: 3,num: 11},
            {_id: 377,donor: 'donor2',recipient: 'recipient0',slot: 8,num: 11},
            {_id: 378,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 2},
            {_id: 379,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 14},
            {_id: 380,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 11},
            {_id: 381,donor: 'donor2',recipient: 'recipient0',slot: 31,num: 17},
            {_id: 382,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 20},
            {_id: 383,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 17},
            {_id: 384,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 2},
        ],
    },
    { // 51
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 179 },u: [{ $set: { num: 17 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 385,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 4},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 251 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 28},update: [{ $set: {donor: 'donor2',recipient: 'recipient0'} },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 34},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 277 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 165 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 27},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 29},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 168 },limit: 1}]}},
        ],
    },
    { // 52
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 386,donor: 'donor1',recipient: 'recipient0',slot: 24,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 387,donor: 'donor2',recipient: 'recipient0',slot: 29,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 388,donor: 'donor1',recipient: 'recipient0',slot: 30,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 389,donor: 'donor1',recipient: 'recipient0',slot: 5,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 390,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 391,donor: 'donor0',recipient: 'recipient0',slot: 6,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 392,donor: 'donor2',recipient: 'recipient0',slot: 23,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 393,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 394,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 395,donor: 'donor0',recipient: 'recipient0',slot: 17,num: 19}]}},
        ],
    },
    { // 53
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 396,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 9},
            {_id: 397,donor: 'donor2',recipient: 'recipient0',slot: 9,num: 12},
            {_id: 398,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 14},
            {_id: 399,donor: 'donor1',recipient: 'recipient0',slot: 28,num: 16},
            {_id: 400,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 19},
            {_id: 401,donor: 'donor1',recipient: 'recipient0',slot: 41,num: 2},
            {_id: 402,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 4},
            {_id: 403,donor: 'donor0',recipient: 'recipient0',slot: 43,num: 15},
            {_id: 404,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 4},
            {_id: 405,donor: 'donor1',recipient: 'recipient0',slot: 9,num: 11},
        ],
    },
    { // 54
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 25},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 406,donor: 'donor2',recipient: 'recipient0',slot: 47,num: 16}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 3},u: {donor: 'donor1',recipient: 'recipient0',slot: 2,num: 16}}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 407,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 17}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 384 },u: [{ $set: { num: 14 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 25},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 243 },u: { $set: { num: 9 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 65 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 408,donor: 'donor1',recipient: 'recipient0',slot: 8,num: 5}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 46},remove: true}},
        ],
    },
    { // 55
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 409,donor: 'donor1',recipient: 'recipient0',slot: 36,num: 6},
            {_id: 410,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 8},
            {_id: 411,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 11},
            {_id: 412,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 1},
            {_id: 413,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 19},
            {_id: 414,donor: 'donor0',recipient: 'recipient0',slot: 35,num: 20},
            {_id: 415,donor: 'donor0',recipient: 'recipient0',slot: 17,num: 13},
            {_id: 416,donor: 'donor0',recipient: 'recipient0',slot: 28,num: 1},
            {_id: 417,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 9},
            {_id: 418,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 7},
        ],
    },
    { // 56
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 10},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 419,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 420,donor: 'donor0',recipient: 'recipient0',slot: 35,num: 2}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 302 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 399 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 279 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 421,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 5}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 30},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 370 },u: { $set: { num: 1 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 422,donor: 'donor2',recipient: 'recipient0',slot: 6,num: 5},update: { $inc: { num: 0 } },new: true,upsert: true}},
        ],
    },
    { // 57
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 423,donor: 'donor0',recipient: 'recipient0',slot: 4,num: 1},
            {_id: 424,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 18},
            {_id: 425,donor: 'donor2',recipient: 'recipient0',slot: 45,num: 11},
            {_id: 426,donor: 'donor2',recipient: 'recipient0',slot: 8,num: 14},
            {_id: 427,donor: 'donor1',recipient: 'recipient0',slot: 19,num: 20},
            {_id: 428,donor: 'donor0',recipient: 'recipient0',slot: 42,num: 3},
            {_id: 429,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 18},
            {_id: 430,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 16},
            {_id: 431,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 2},
            {_id: 432,donor: 'donor1',recipient: 'recipient0',slot: 30,num: 13},
        ],
    },
    { // 58
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 9},update: {$set: { donor: 'donor1' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 18},update: {$set: {donor: 'donor1',recipient: 'recipient0'},$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 433,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 434,donor: 'donor0',recipient: 'recipient0',slot: 30,num: 16}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 335 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 61 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 6},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 127 },u: { $set: { num: 15 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 101 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 435,donor: 'donor0',recipient: 'recipient0',slot: 35,num: 11}]}},
        ],
    },
    { // 59
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 436,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 9},
            {_id: 437,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 13},
            {_id: 438,donor: 'donor1',recipient: 'recipient0',slot: 16,num: 15},
            {_id: 439,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 1},
            {_id: 440,donor: 'donor1',recipient: 'recipient0',slot: 32,num: 4},
            {_id: 441,donor: 'donor2',recipient: 'recipient0',slot: 4,num: 3},
            {_id: 442,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 15},
            {_id: 443,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 3},
            {_id: 444,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 13},
            {_id: 445,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 9},
        ],
    },
    { // 60
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 446,donor: 'donor0',recipient: 'recipient0',slot: 42,num: 14},
            {_id: 447,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 18},
            {_id: 448,donor: 'donor0',recipient: 'recipient0',slot: 10,num: 12},
            {_id: 449,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 16},
            {_id: 450,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 8},
            {_id: 451,donor: 'donor0',recipient: 'recipient0',slot: 29,num: 4},
            {_id: 452,donor: 'donor0',recipient: 'recipient0',slot: 29,num: 4},
            {_id: 453,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 5},
            {_id: 454,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 18},
            {_id: 455,donor: 'donor1',recipient: 'recipient0',slot: 32,num: 14},
        ],
    },
    { // 61
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 456,donor: 'donor1',recipient: 'recipient0',slot: 3,num: 3},
            {_id: 457,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 1},
            {_id: 458,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 14},
            {_id: 459,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 2},
            {_id: 460,donor: 'donor2',recipient: 'recipient0',slot: 2,num: 3},
            {_id: 461,donor: 'donor0',recipient: 'recipient0',slot: 1,num: 7},
            {_id: 462,donor: 'donor0',recipient: 'recipient0',slot: 2,num: 4},
            {_id: 463,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 12},
            {_id: 464,donor: 'donor1',recipient: 'recipient0',slot: 6,num: 2},
            {_id: 465,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 19},
        ],
    },
    { // 62
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 271 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 203 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 50 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 36},update: [{ $set: { recipient: 'recipient0' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 466,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 432 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 88 },u: [{ $set: { num: 1 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 419 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 368 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 261 },u: { $set: { num: 11 } }}]}},
        ],
    },
    { // 63
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 467,donor: 'donor0',recipient: 'recipient0',slot: 36,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 468,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 469,donor: 'donor0',recipient: 'recipient0',slot: 42,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 470,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 471,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 472,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 473,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 474,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 475,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 476,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 9}]}},
        ],
    },
    { // 64
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 477,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 478,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 479,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 480,donor: 'donor1',recipient: 'recipient0',slot: 4,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 481,donor: 'donor1',recipient: 'recipient0',slot: 12,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 482,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 483,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 484,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 485,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 486,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 17}]}},
        ],
    },
    { // 65
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 487,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 488,donor: 'donor1',recipient: 'recipient0',slot: 6,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 489,donor: 'donor1',recipient: 'recipient0',slot: 33,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 490,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 491,donor: 'donor2',recipient: 'recipient0',slot: 14,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 492,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 493,donor: 'donor0',recipient: 'recipient0',slot: 11,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 494,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 495,donor: 'donor1',recipient: 'recipient0',slot: 27,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 496,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 12}]}},
        ],
    },
    { // 66
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 16},u: {donor: 'donor2',recipient: 'recipient0',slot: 17,num: 3}}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 27},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',recipient: 'recipient0',slot: 8},u: {donor: 'donor2',recipient: 'recipient0',slot: 10,num: 12}}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 14},u: {donor: 'donor1',recipient: 'recipient0',slot: 29,num: 16}}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 497,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 5}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 498,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 6},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 488 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',recipient: 'recipient0',slot: 48},u: {donor: 'donor1',recipient: 'recipient0',slot: 48,num: 5}}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 489 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 24},u: [{ $set: { recipient: 'recipient0' } }]}]}},
        ],
    },
    { // 67
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 137 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 43},u: {donor: 'donor1',recipient: 'recipient0',slot: 33,num: 10}}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 2},u: [{ $set: { recipient: 'recipient0' } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 177 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 260 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 8 },u: [{ $set: { num: 3 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 499,donor: 'donor2',recipient: 'recipient0',slot: 6,num: 12}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 446 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 35},u: {donor: 'donor0',recipient: 'recipient0',slot: 33,num: 12}}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 8},u: [{ $set: { donor: 'donor2' } }]}]}},
        ],
    },
    { // 68
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 500,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 10},
            {_id: 501,donor: 'donor1',recipient: 'recipient0',slot: 8,num: 16},
            {_id: 502,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 14},
            {_id: 503,donor: 'donor0',recipient: 'recipient0',slot: 14,num: 17},
            {_id: 504,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 10},
            {_id: 505,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 1},
            {_id: 506,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 3},
            {_id: 507,donor: 'donor2',recipient: 'recipient0',slot: 18,num: 9},
            {_id: 508,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 7},
            {_id: 509,donor: 'donor1',recipient: 'recipient0',slot: 8,num: 1},
        ],
    },
    { // 69
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 426 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 285 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 383 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 19},u: {donor: 'donor1',recipient: 'recipient0',slot: 6,num: 6}}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 163 },u: [{ $set: { num: 3 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 510,donor: 'donor0',recipient: 'recipient0',slot: 22,num: 11}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 23},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 39},u: { $set: {donor: 'donor0',recipient: 'recipient0'} }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 511,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 512,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 11}]}},
        ],
    },
    { // 70
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 9},u: { $set: { num: 5 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 30},u: { $set: { donor: 'donor1' } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 14 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 513,donor: 'donor0',recipient: 'recipient0',slot: 0,num: 16}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 235 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 514,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 2}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 326 },u: [{ $set: { num: 2 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 246 },u: [{ $set: { num: 8 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 46},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 514 },u: { $set: { num: 18 } }}]}},
        ],
    },
    { // 71
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 515,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 516,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 517,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 518,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 519,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 520,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 521,donor: 'donor1',recipient: 'recipient0',slot: 19,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 522,donor: 'donor0',recipient: 'recipient0',slot: 18,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 523,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 524,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 7}]}},
        ],
    },
    { // 72
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 525,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 4}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 524 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 452 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 280 },u: [{ $set: { num: 2 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 82 },u: [{ $set: { num: 9 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 39},u: { $set: { recipient: 'recipient0' } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 492 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 23},u: { $set: { recipient: 'recipient0' } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 503 },u: { $set: { num: 17 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 388 },u: [{ $set: { num: 20 } }]}]}},
        ],
    },
    { // 73
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 526,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 18},
            {_id: 527,donor: 'donor1',recipient: 'recipient0',slot: 47,num: 18},
            {_id: 528,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 7},
            {_id: 529,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 19},
            {_id: 530,donor: 'donor1',recipient: 'recipient0',slot: 20,num: 6},
            {_id: 531,donor: 'donor2',recipient: 'recipient0',slot: 4,num: 12},
            {_id: 532,donor: 'donor2',recipient: 'recipient0',slot: 14,num: 9},
            {_id: 533,donor: 'donor2',recipient: 'recipient0',slot: 46,num: 13},
            {_id: 534,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 14},
            {_id: 535,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 9},
        ],
    },
    { // 74
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 536,donor: 'donor0',recipient: 'recipient0',slot: 8,num: 18},
            {_id: 537,donor: 'donor0',recipient: 'recipient0',slot: 25,num: 15},
            {_id: 538,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 11},
            {_id: 539,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 4},
            {_id: 540,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 13},
            {_id: 541,donor: 'donor0',recipient: 'recipient0',slot: 37,num: 19},
            {_id: 542,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 9},
            {_id: 543,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 3},
            {_id: 544,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 16},
            {_id: 545,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 9},
        ],
    },
    { // 75
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 546,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 2},
            {_id: 547,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 20},
            {_id: 548,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 14},
            {_id: 549,donor: 'donor0',recipient: 'recipient0',slot: 19,num: 5},
            {_id: 550,donor: 'donor1',recipient: 'recipient0',slot: 24,num: 19},
            {_id: 551,donor: 'donor1',recipient: 'recipient0',slot: 12,num: 20},
            {_id: 552,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 6},
            {_id: 553,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 10},
            {_id: 554,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 7},
            {_id: 555,donor: 'donor0',recipient: 'recipient0',slot: 2,num: 6},
        ],
    },
    { // 76
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 556,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 12},
            {_id: 557,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 11},
            {_id: 558,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 18},
            {_id: 559,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 11},
            {_id: 560,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 14},
            {_id: 561,donor: 'donor2',recipient: 'recipient0',slot: 31,num: 19},
            {_id: 562,donor: 'donor2',recipient: 'recipient0',slot: 23,num: 20},
            {_id: 563,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 8},
            {_id: 564,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 4},
            {_id: 565,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 11},
        ],
    },
    { // 77
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 566,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 19},
            {_id: 567,donor: 'donor0',recipient: 'recipient0',slot: 28,num: 19},
            {_id: 568,donor: 'donor1',recipient: 'recipient0',slot: 24,num: 16},
            {_id: 569,donor: 'donor2',recipient: 'recipient0',slot: 9,num: 8},
            {_id: 570,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 1},
            {_id: 571,donor: 'donor2',recipient: 'recipient0',slot: 4,num: 3},
            {_id: 572,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 4},
            {_id: 573,donor: 'donor0',recipient: 'recipient0',slot: 37,num: 16},
            {_id: 574,donor: 'donor0',recipient: 'recipient0',slot: 37,num: 1},
            {_id: 575,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 4},
        ],
    },
    { // 78
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 576,donor: 'donor0',recipient: 'recipient0',slot: 21,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 577,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 578,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 579,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 580,donor: 'donor1',recipient: 'recipient0',slot: 9,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 581,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 582,donor: 'donor2',recipient: 'recipient0',slot: 31,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 583,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 584,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 585,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 12}]}},
        ],
    },
    { // 79
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 586,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 9},
            {_id: 587,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 20},
            {_id: 588,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 19},
            {_id: 589,donor: 'donor1',recipient: 'recipient0',slot: 9,num: 4},
            {_id: 590,donor: 'donor2',recipient: 'recipient0',slot: 7,num: 13},
            {_id: 591,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 20},
            {_id: 592,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 16},
            {_id: 593,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 13},
            {_id: 594,donor: 'donor2',recipient: 'recipient0',slot: 29,num: 10},
            {_id: 595,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 3},
        ],
    },
    { // 80
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 596,donor: 'donor0',recipient: 'recipient0',slot: 1,num: 16},
            {_id: 597,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 5},
            {_id: 598,donor: 'donor0',recipient: 'recipient0',slot: 25,num: 15},
            {_id: 599,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 14},
            {_id: 600,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 9},
            {_id: 601,donor: 'donor2',recipient: 'recipient0',slot: 17,num: 15},
            {_id: 602,donor: 'donor0',recipient: 'recipient0',slot: 3,num: 7},
            {_id: 603,donor: 'donor0',recipient: 'recipient0',slot: 38,num: 17},
            {_id: 604,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 7},
            {_id: 605,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 10},
        ],
    },
    { // 81
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 606,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 607,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 608,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 609,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 610,donor: 'donor2',recipient: 'recipient0',slot: 18,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 611,donor: 'donor0',recipient: 'recipient0',slot: 0,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 612,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 613,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 614,donor: 'donor1',recipient: 'recipient0',slot: 28,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 615,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 9}]}},
        ],
    },
    { // 82
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 616,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 7},
            {_id: 617,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 19},
            {_id: 618,donor: 'donor2',recipient: 'recipient0',slot: 45,num: 11},
            {_id: 619,donor: 'donor2',recipient: 'recipient0',slot: 22,num: 13},
            {_id: 620,donor: 'donor1',recipient: 'recipient0',slot: 41,num: 4},
            {_id: 621,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 6},
            {_id: 622,donor: 'donor1',recipient: 'recipient0',slot: 23,num: 5},
            {_id: 623,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 1},
            {_id: 624,donor: 'donor2',recipient: 'recipient0',slot: 7,num: 13},
            {_id: 625,donor: 'donor0',recipient: 'recipient0',slot: 36,num: 17},
        ],
    },
    { // 83
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 626,donor: 'donor0',recipient: 'recipient0',slot: 14,num: 6}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 531 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 25},u: {donor: 'donor0',recipient: 'recipient0',slot: 19,num: 9}}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 627,donor: 'donor1',recipient: 'recipient0',slot: 47,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 628,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 629,donor: 'donor1',recipient: 'recipient0',slot: 4,num: 11}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 630,donor: 'donor1',recipient: 'recipient0',slot: 35,num: 7},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 29},update: {$set: { donor: 'donor1' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 4},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 631,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 19}]}},
        ],
    },
    { // 84
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 632,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 633,donor: 'donor2',recipient: 'recipient0',slot: 39,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 634,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 635,donor: 'donor0',recipient: 'recipient0',slot: 14,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 636,donor: 'donor1',recipient: 'recipient0',slot: 19,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 637,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 638,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 639,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 640,donor: 'donor2',recipient: 'recipient0',slot: 22,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 641,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 17}]}},
        ],
    },
    { // 85
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 642,donor: 'donor0',recipient: 'recipient0',slot: 25,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 643,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 644,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 645,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 646,donor: 'donor2',recipient: 'recipient0',slot: 14,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 647,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 648,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 649,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 650,donor: 'donor2',recipient: 'recipient0',slot: 19,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 651,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 14}]}},
        ],
    },
    { // 86
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 236 },u: { $set: { num: 8 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 36},u: { $set: { recipient: 'recipient0' } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 423 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 652,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 12}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 27},u: { $set: { num: 3 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 349 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 653,donor: 'donor1',recipient: 'recipient0',slot: 6,num: 11}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 654,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 20},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 180 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 45},u: { $set: {donor: 'donor0',recipient: 'recipient0'} }}]}},
        ],
    },
    { // 87
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 544 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 416 },u: [{ $set: { num: 10 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 175 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 655,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 18}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 0},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 537 },u: [{ $set: { num: 3 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 500 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 168 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 2},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 656,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 16}]}},
        ],
    },
    { // 88
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 657,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 9},
            {_id: 658,donor: 'donor0',recipient: 'recipient0',slot: 25,num: 16},
            {_id: 659,donor: 'donor0',recipient: 'recipient0',slot: 38,num: 3},
            {_id: 660,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 17},
            {_id: 661,donor: 'donor0',recipient: 'recipient0',slot: 22,num: 13},
            {_id: 662,donor: 'donor1',recipient: 'recipient0',slot: 36,num: 10},
            {_id: 663,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 19},
            {_id: 664,donor: 'donor2',recipient: 'recipient0',slot: 9,num: 2},
            {_id: 665,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 7},
            {_id: 666,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 13},
        ],
    },
    { // 89
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 667,donor: 'donor2',recipient: 'recipient0',slot: 25,num: 9},
            {_id: 668,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 18},
            {_id: 669,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 20},
            {_id: 670,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 13},
            {_id: 671,donor: 'donor1',recipient: 'recipient0',slot: 20,num: 18},
            {_id: 672,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 5},
            {_id: 673,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 8},
            {_id: 674,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 8},
            {_id: 675,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 1},
            {_id: 676,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 1},
        ],
    },
    { // 90
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 33},update: {$set: { num: 6 },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 677,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 11}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 588 },u: [{ $set: { num: 19 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 567 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 342 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 1},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 559 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 580 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 291 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 678,donor: 'donor2',recipient: 'recipient0',slot: 2,num: 10},update: { $inc: { num: 0 } },new: true,upsert: true}},
        ],
    },
    { // 91
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 304 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 571 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 35},u: { $set: { donor: 'donor1' } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 40},update: [{ $set: { num: 14 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 679,donor: 'donor0',recipient: 'recipient0',slot: 2,num: 3}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 35},u: {donor: 'donor2',recipient: 'recipient0',slot: 2,num: 8}}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 29},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 680,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 5},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 681,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 10}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 49},remove: true}},
        ],
    },
    { // 92
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 12},update: {$set: { num: 6 },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 33},u: { $set: { recipient: 'recipient0' } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 4},u: {donor: 'donor2',recipient: 'recipient0',slot: 6,num: 14}}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 326 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 28},u: [{ $set: {donor: 'donor0',recipient: 'recipient0'} }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 2},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 682,donor: 'donor2',recipient: 'recipient0',slot: 18,num: 15},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 683,donor: 'donor0',recipient: 'recipient0',slot: 22,num: 15}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 507 },u: { $set: { num: 8 } }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 684,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 16}]}},
        ],
    },
    { // 93
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 685,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 686,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 687,donor: 'donor2',recipient: 'recipient0',slot: 45,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 688,donor: 'donor2',recipient: 'recipient0',slot: 29,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 689,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 690,donor: 'donor2',recipient: 'recipient0',slot: 38,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 691,donor: 'donor2',recipient: 'recipient0',slot: 6,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 692,donor: 'donor2',recipient: 'recipient0',slot: 23,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 693,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 694,donor: 'donor2',recipient: 'recipient0',slot: 13,num: 10}]}},
        ],
    },
    { // 94
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 695,donor: 'donor0',recipient: 'recipient0',slot: 32,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 696,donor: 'donor0',recipient: 'recipient0',slot: 41,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 697,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 698,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 699,donor: 'donor0',recipient: 'recipient0',slot: 28,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 700,donor: 'donor0',recipient: 'recipient0',slot: 28,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 701,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 702,donor: 'donor0',recipient: 'recipient0',slot: 6,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 703,donor: 'donor1',recipient: 'recipient0',slot: 35,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 704,donor: 'donor2',recipient: 'recipient0',slot: 7,num: 16}]}},
        ],
    },
    { // 95
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 705,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 706,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 707,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 708,donor: 'donor2',recipient: 'recipient0',slot: 13,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 709,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 710,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 711,donor: 'donor1',recipient: 'recipient0',slot: 19,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 712,donor: 'donor2',recipient: 'recipient0',slot: 2,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 713,donor: 'donor2',recipient: 'recipient0',slot: 2,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 714,donor: 'donor0',recipient: 'recipient0',slot: 14,num: 9}]}},
        ],
    },
    { // 96
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 715,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 19},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 26},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 16},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 716,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 3}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 10},update: [{ $set: { num: 10 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 717,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 718,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 14},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 13},u: [{ $set: { donor: 'donor1' } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 719,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 720,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 19}]}},
        ],
    },
    { // 97
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 721,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 12},
            {_id: 722,donor: 'donor1',recipient: 'recipient0',slot: 9,num: 3},
            {_id: 723,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 13},
            {_id: 724,donor: 'donor2',recipient: 'recipient0',slot: 21,num: 18},
            {_id: 725,donor: 'donor0',recipient: 'recipient0',slot: 42,num: 6},
            {_id: 726,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 10},
            {_id: 727,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 7},
            {_id: 728,donor: 'donor0',recipient: 'recipient0',slot: 43,num: 5},
            {_id: 729,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 7},
            {_id: 730,donor: 'donor0',recipient: 'recipient0',slot: 41,num: 3},
        ],
    },
    { // 98
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 731,donor: 'donor0',recipient: 'recipient0',slot: 29,num: 19}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 732,donor: 'donor1',recipient: 'recipient0',slot: 23,num: 18},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 37},u: {donor: 'donor1',recipient: 'recipient0',slot: 29,num: 11}}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 93 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 733,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 8},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 7},update: [{ $set: {donor: 'donor2',recipient: 'recipient0'} },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 734,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 10}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 90 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 8},update: {$set: { donor: 'donor2' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 10},update: {$set: { donor: 'donor1' },$inc: { dummy: 1 }},new: false}},
        ],
    },
    { // 99
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 735,donor: 'donor1',recipient: 'recipient0',slot: 27,num: 12},
            {_id: 736,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 10},
            {_id: 737,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 20},
            {_id: 738,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 13},
            {_id: 739,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 1},
            {_id: 740,donor: 'donor1',recipient: 'recipient0',slot: 47,num: 2},
            {_id: 741,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 10},
            {_id: 742,donor: 'donor0',recipient: 'recipient0',slot: 8,num: 15},
            {_id: 743,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 18},
            {_id: 744,donor: 'donor1',recipient: 'recipient0',slot: 38,num: 19},
        ],
    },
    { // 100
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 745,donor: 'donor0',recipient: 'recipient0',slot: 25,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 746,donor: 'donor2',recipient: 'recipient0',slot: 25,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 747,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 748,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 749,donor: 'donor2',recipient: 'recipient0',slot: 8,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 750,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 751,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 752,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 753,donor: 'donor0',recipient: 'recipient0',slot: 8,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 754,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 6}]}},
        ],
    },
    { // 101
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 755,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 20},
            {_id: 756,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 8},
            {_id: 757,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 12},
            {_id: 758,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 3},
            {_id: 759,donor: 'donor2',recipient: 'recipient0',slot: 9,num: 10},
            {_id: 760,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 19},
            {_id: 761,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 5},
            {_id: 762,donor: 'donor0',recipient: 'recipient0',slot: 38,num: 6},
            {_id: 763,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 2},
            {_id: 764,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 8},
        ],
    },
    { // 102
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 765,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 766,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 767,donor: 'donor2',recipient: 'recipient0',slot: 31,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 768,donor: 'donor1',recipient: 'recipient0',slot: 12,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 769,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 770,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 771,donor: 'donor1',recipient: 'recipient0',slot: 6,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 772,donor: 'donor2',recipient: 'recipient0',slot: 8,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 773,donor: 'donor0',recipient: 'recipient0',slot: 11,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 774,donor: 'donor1',recipient: 'recipient0',slot: 18,num: 2}]}},
        ],
    },
    { // 103
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 775,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 776,donor: 'donor0',recipient: 'recipient0',slot: 37,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 777,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 13}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 778,donor: 'donor1',recipient: 'recipient0',slot: 35,num: 19},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 779,donor: 'donor0',recipient: 'recipient0',slot: 10,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 780,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 16}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 22},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',recipient: 'recipient0',slot: 10},u: {donor: 'donor0',recipient: 'recipient0',slot: 5,num: 2}}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 0},update: {$set: { num: 1 },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 144 },limit: 1}]}},
        ],
    },
    { // 104
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 781,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 782,donor: 'donor2',recipient: 'recipient0',slot: 6,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 783,donor: 'donor0',recipient: 'recipient0',slot: 42,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 784,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 785,donor: 'donor1',recipient: 'recipient0',slot: 32,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 786,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 787,donor: 'donor2',recipient: 'recipient0',slot: 17,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 788,donor: 'donor0',recipient: 'recipient0',slot: 11,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 789,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 790,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 15}]}},
        ],
    },
    { // 105
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 791,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 792,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 793,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 794,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 795,donor: 'donor1',recipient: 'recipient0',slot: 32,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 796,donor: 'donor0',recipient: 'recipient0',slot: 29,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 797,donor: 'donor0',recipient: 'recipient0',slot: 45,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 798,donor: 'donor0',recipient: 'recipient0',slot: 0,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 799,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 800,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 6}]}},
        ],
    },
    { // 106
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 801,donor: 'donor1',recipient: 'recipient0',slot: 24,num: 17},
            {_id: 802,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 1},
            {_id: 803,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 8},
            {_id: 804,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 10},
            {_id: 805,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 10},
            {_id: 806,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 16},
            {_id: 807,donor: 'donor0',recipient: 'recipient0',slot: 0,num: 6},
            {_id: 808,donor: 'donor2',recipient: 'recipient0',slot: 17,num: 13},
            {_id: 809,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 11},
            {_id: 810,donor: 'donor0',recipient: 'recipient0',slot: 12,num: 19},
        ],
    },
    { // 107
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 811,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 2},
            {_id: 812,donor: 'donor2',recipient: 'recipient0',slot: 7,num: 8},
            {_id: 813,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 17},
            {_id: 814,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 13},
            {_id: 815,donor: 'donor0',recipient: 'recipient0',slot: 17,num: 8},
            {_id: 816,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 2},
            {_id: 817,donor: 'donor0',recipient: 'recipient0',slot: 8,num: 7},
            {_id: 818,donor: 'donor1',recipient: 'recipient0',slot: 5,num: 19},
            {_id: 819,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 8},
            {_id: 820,donor: 'donor0',recipient: 'recipient0',slot: 37,num: 18},
        ],
    },
    { // 108
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 735 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 4},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 821,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 1},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 8},u: { $set: {donor: 'donor1',recipient: 'recipient0'} }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 577 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 571 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 631 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 17},u: {donor: 'donor0',recipient: 'recipient0',slot: 2,num: 16}}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 822,donor: 'donor0',recipient: 'recipient0',slot: 18,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 823,donor: 'donor2',recipient: 'recipient0',slot: 14,num: 7}]}},
        ],
    },
    { // 109
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 824,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 16},
            {_id: 825,donor: 'donor0',recipient: 'recipient0',slot: 6,num: 17},
            {_id: 826,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 12},
            {_id: 827,donor: 'donor0',recipient: 'recipient0',slot: 10,num: 3},
            {_id: 828,donor: 'donor2',recipient: 'recipient0',slot: 45,num: 5},
            {_id: 829,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 10},
            {_id: 830,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 11},
            {_id: 831,donor: 'donor0',recipient: 'recipient0',slot: 8,num: 10},
            {_id: 832,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 17},
            {_id: 833,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 17},
        ],
    },
    { // 110
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 834,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 6},
            {_id: 835,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 10},
            {_id: 836,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 15},
            {_id: 837,donor: 'donor1',recipient: 'recipient0',slot: 38,num: 6},
            {_id: 838,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 14},
            {_id: 839,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 20},
            {_id: 840,donor: 'donor0',recipient: 'recipient0',slot: 1,num: 9},
            {_id: 841,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 20},
            {_id: 842,donor: 'donor0',recipient: 'recipient0',slot: 10,num: 2},
            {_id: 843,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 3},
        ],
    },
    { // 111
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 844,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 845,donor: 'donor1',recipient: 'recipient0',slot: 5,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 846,donor: 'donor2',recipient: 'recipient0',slot: 15,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 847,donor: 'donor2',recipient: 'recipient0',slot: 17,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 848,donor: 'donor1',recipient: 'recipient0',slot: 16,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 849,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 850,donor: 'donor0',recipient: 'recipient0',slot: 19,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 851,donor: 'donor2',recipient: 'recipient0',slot: 46,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 852,donor: 'donor0',recipient: 'recipient0',slot: 34,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 853,donor: 'donor0',recipient: 'recipient0',slot: 0,num: 7}]}},
        ],
    },
    { // 112
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 854,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 855,donor: 'donor2',recipient: 'recipient0',slot: 38,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 856,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 857,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 858,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 859,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 860,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 861,donor: 'donor0',recipient: 'recipient0',slot: 38,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 862,donor: 'donor2',recipient: 'recipient0',slot: 7,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 863,donor: 'donor2',recipient: 'recipient0',slot: 32,num: 8}]}},
        ],
    },
    { // 113
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 864,donor: 'donor2',recipient: 'recipient0',slot: 8,num: 4},
            {_id: 865,donor: 'donor1',recipient: 'recipient0',slot: 30,num: 15},
            {_id: 866,donor: 'donor1',recipient: 'recipient0',slot: 33,num: 20},
            {_id: 867,donor: 'donor2',recipient: 'recipient0',slot: 29,num: 15},
            {_id: 868,donor: 'donor0',recipient: 'recipient0',slot: 29,num: 14},
            {_id: 869,donor: 'donor2',recipient: 'recipient0',slot: 15,num: 18},
            {_id: 870,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 16},
            {_id: 871,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 6},
            {_id: 872,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 11},
            {_id: 873,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 9},
        ],
    },
    { // 114
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 874,donor: 'donor1',recipient: 'recipient0',slot: 9,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 875,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 876,donor: 'donor1',recipient: 'recipient0',slot: 19,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 877,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 878,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 879,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 880,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 881,donor: 'donor0',recipient: 'recipient0',slot: 11,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 882,donor: 'donor0',recipient: 'recipient0',slot: 22,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 883,donor: 'donor0',recipient: 'recipient0',slot: 6,num: 7}]}},
        ],
    },
    { // 115
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 10},u: [{ $set: { recipient: 'recipient0' } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 408 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 32},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 884,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 7}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 586 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 8},u: [{ $set: {donor: 'donor1',recipient: 'recipient0'} }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 41},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 885,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 10},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 221 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 28},remove: true}},
        ],
    },
    { // 116
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 886,donor: 'donor0',recipient: 'recipient0',slot: 28,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 887,donor: 'donor0',recipient: 'recipient0',slot: 21,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 888,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 889,donor: 'donor2',recipient: 'recipient0',slot: 2,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 890,donor: 'donor2',recipient: 'recipient0',slot: 21,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 891,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 892,donor: 'donor0',recipient: 'recipient0',slot: 21,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 893,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 894,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 895,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 3}]}},
        ],
    },
    { // 117
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 61 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 30},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 896,donor: 'donor0',recipient: 'recipient0',slot: 14,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 897,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 20}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 573 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 19},update: [{ $set: {donor: 'donor2',recipient: 'recipient0'} },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 51 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 898,donor: 'donor2',recipient: 'recipient0',slot: 6,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 899,donor: 'donor0',recipient: 'recipient0',slot: 45,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 900,donor: 'donor1',recipient: 'recipient0',slot: 23,num: 15}]}},
        ],
    },
    { // 118
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 901,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 1},
            {_id: 902,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 7},
            {_id: 903,donor: 'donor1',recipient: 'recipient0',slot: 27,num: 2},
            {_id: 904,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 20},
            {_id: 905,donor: 'donor0',recipient: 'recipient0',slot: 12,num: 19},
            {_id: 906,donor: 'donor1',recipient: 'recipient0',slot: 16,num: 8},
            {_id: 907,donor: 'donor0',recipient: 'recipient0',slot: 30,num: 13},
            {_id: 908,donor: 'donor0',recipient: 'recipient0',slot: 11,num: 13},
            {_id: 909,donor: 'donor2',recipient: 'recipient0',slot: 25,num: 14},
            {_id: 910,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 6},
        ],
    },
    { // 119
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 911,donor: 'donor0',recipient: 'recipient0',slot: 10,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 912,donor: 'donor2',recipient: 'recipient0',slot: 46,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 913,donor: 'donor0',recipient: 'recipient0',slot: 4,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 914,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 915,donor: 'donor0',recipient: 'recipient0',slot: 8,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 916,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 917,donor: 'donor0',recipient: 'recipient0',slot: 2,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 918,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 919,donor: 'donor1',recipient: 'recipient0',slot: 16,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 920,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 16}]}},
        ],
    },
    { // 120
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 30},u: { $set: { recipient: 'recipient0' } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 25},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 347 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 6 },u: { $set: { num: 11 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 755 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 36},update: {$set: { donor: 'donor1' },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 455 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 921,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 12},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 30},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 922,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 14}]}},
        ],
    },
    { // 121
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 923,donor: 'donor2',recipient: 'recipient0',slot: 31,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 924,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 925,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 926,donor: 'donor0',recipient: 'recipient0',slot: 21,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 927,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 928,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 929,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 930,donor: 'donor0',recipient: 'recipient0',slot: 18,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 931,donor: 'donor1',recipient: 'recipient0',slot: 38,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 932,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 15}]}},
        ],
    },
    { // 122
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 45 },u: { $set: { num: 15 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 86 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 41},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 40},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 933,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 934,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 16}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 43},u: { $set: { num: 10 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 128 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 358 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 935,donor: 'donor2',recipient: 'recipient0',slot: 8,num: 7}]}},
        ],
    },
    { // 123
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 936,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 937,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 938,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 939,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 940,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 941,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 942,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 943,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 944,donor: 'donor0',recipient: 'recipient0',slot: 22,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 945,donor: 'donor2',recipient: 'recipient0',slot: 6,num: 12}]}},
        ],
    },
    { // 124
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 946,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 947,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 948,donor: 'donor2',recipient: 'recipient0',slot: 38,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 949,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 950,donor: 'donor1',recipient: 'recipient0',slot: 20,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 951,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 952,donor: 'donor2',recipient: 'recipient0',slot: 2,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 953,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 954,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 955,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 3}]}},
        ],
    },
    { // 125
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 956,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 957,donor: 'donor1',recipient: 'recipient0',slot: 28,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 958,donor: 'donor2',recipient: 'recipient0',slot: 39,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 959,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 960,donor: 'donor0',recipient: 'recipient0',slot: 10,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 961,donor: 'donor2',recipient: 'recipient0',slot: 9,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 962,donor: 'donor0',recipient: 'recipient0',slot: 22,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 963,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 964,donor: 'donor2',recipient: 'recipient0',slot: 25,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 965,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 4}]}},
        ],
    },
    { // 126
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 966,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 967,donor: 'donor1',recipient: 'recipient0',slot: 25,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 968,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 969,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 970,donor: 'donor0',recipient: 'recipient0',slot: 12,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 971,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 972,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 973,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 974,donor: 'donor0',recipient: 'recipient0',slot: 35,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 975,donor: 'donor0',recipient: 'recipient0',slot: 42,num: 15}]}},
        ],
    },
    { // 127
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 976,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 10}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 950 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 977,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 12}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 0},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 905 },u: { $set: { num: 7 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 978,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 12},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 158 },u: { $set: { num: 17 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 6},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 979,donor: 'donor2',recipient: 'recipient0',slot: 14,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 980,donor: 'donor0',recipient: 'recipient0',slot: 19,num: 1}]}},
        ],
    },
    { // 128
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 981,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 982,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 983,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 984,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 985,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 986,donor: 'donor1',recipient: 'recipient0',slot: 38,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 987,donor: 'donor0',recipient: 'recipient0',slot: 14,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 988,donor: 'donor0',recipient: 'recipient0',slot: 30,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 989,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 990,donor: 'donor2',recipient: 'recipient0',slot: 32,num: 20}]}},
        ],
    },
    { // 129
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 991,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 992,donor: 'donor0',recipient: 'recipient0',slot: 42,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 993,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 994,donor: 'donor1',recipient: 'recipient0',slot: 44,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 995,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 996,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 997,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 998,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 999,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1000,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 10}]}},
        ],
    },
    { // 130
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1001,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 19},
            {_id: 1002,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 11},
            {_id: 1003,donor: 'donor0',recipient: 'recipient0',slot: 35,num: 8},
            {_id: 1004,donor: 'donor2',recipient: 'recipient0',slot: 21,num: 2},
            {_id: 1005,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 20},
            {_id: 1006,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 5},
            {_id: 1007,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 10},
            {_id: 1008,donor: 'donor1',recipient: 'recipient0',slot: 40,num: 12},
            {_id: 1009,donor: 'donor0',recipient: 'recipient0',slot: 41,num: 11},
            {_id: 1010,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 17},
        ],
    },
    { // 131
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1011,donor: 'donor1',recipient: 'recipient0',slot: 35,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1012,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1013,donor: 'donor1',recipient: 'recipient0',slot: 41,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1014,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1015,donor: 'donor1',recipient: 'recipient0',slot: 40,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1016,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1017,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1018,donor: 'donor2',recipient: 'recipient0',slot: 46,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1019,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1020,donor: 'donor1',recipient: 'recipient0',slot: 30,num: 2}]}},
        ],
    },
    { // 132
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1021,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1022,donor: 'donor1',recipient: 'recipient0',slot: 19,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1023,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1024,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1025,donor: 'donor0',recipient: 'recipient0',slot: 45,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1026,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1027,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1028,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1029,donor: 'donor0',recipient: 'recipient0',slot: 38,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1030,donor: 'donor0',recipient: 'recipient0',slot: 2,num: 13}]}},
        ],
    },
    { // 133
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1031,donor: 'donor2',recipient: 'recipient0',slot: 19,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1032,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1033,donor: 'donor0',recipient: 'recipient0',slot: 12,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1034,donor: 'donor0',recipient: 'recipient0',slot: 41,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1035,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1036,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1037,donor: 'donor0',recipient: 'recipient0',slot: 41,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1038,donor: 'donor1',recipient: 'recipient0',slot: 28,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1039,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1040,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 17}]}},
        ],
    },
    { // 134
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1041,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1042,donor: 'donor1',recipient: 'recipient0',slot: 40,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1043,donor: 'donor0',recipient: 'recipient0',slot: 41,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1044,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1045,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1046,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1047,donor: 'donor0',recipient: 'recipient0',slot: 38,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1048,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1049,donor: 'donor2',recipient: 'recipient0',slot: 19,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1050,donor: 'donor0',recipient: 'recipient0',slot: 38,num: 16}]}},
        ],
    },
    { // 135
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1051,donor: 'donor2',recipient: 'recipient0',slot: 25,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1052,donor: 'donor2',recipient: 'recipient0',slot: 12,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1053,donor: 'donor1',recipient: 'recipient0',slot: 20,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1054,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1055,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1056,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1057,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1058,donor: 'donor1',recipient: 'recipient0',slot: 4,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1059,donor: 'donor1',recipient: 'recipient0',slot: 38,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1060,donor: 'donor0',recipient: 'recipient0',slot: 29,num: 11}]}},
        ],
    },
    { // 136
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1061,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1062,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1063,donor: 'donor2',recipient: 'recipient0',slot: 12,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1064,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1065,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1066,donor: 'donor2',recipient: 'recipient0',slot: 22,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1067,donor: 'donor2',recipient: 'recipient0',slot: 9,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1068,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1069,donor: 'donor0',recipient: 'recipient0',slot: 37,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1070,donor: 'donor2',recipient: 'recipient0',slot: 13,num: 19}]}},
        ],
    },
    { // 137
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1071,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1072,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1073,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1074,donor: 'donor1',recipient: 'recipient0',slot: 36,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1075,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1076,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1077,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1078,donor: 'donor0',recipient: 'recipient0',slot: 22,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1079,donor: 'donor0',recipient: 'recipient0',slot: 28,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1080,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 20}]}},
        ],
    },
    { // 138
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1081,donor: 'donor2',recipient: 'recipient0',slot: 4,num: 17},
            {_id: 1082,donor: 'donor2',recipient: 'recipient0',slot: 13,num: 2},
            {_id: 1083,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 7},
            {_id: 1084,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 17},
            {_id: 1085,donor: 'donor2',recipient: 'recipient0',slot: 8,num: 13},
            {_id: 1086,donor: 'donor0',recipient: 'recipient0',slot: 45,num: 12},
            {_id: 1087,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 11},
            {_id: 1088,donor: 'donor1',recipient: 'recipient0',slot: 35,num: 11},
            {_id: 1089,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 8},
            {_id: 1090,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 18},
        ],
    },
    { // 139
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1091,donor: 'donor1',recipient: 'recipient0',slot: 20,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1092,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 13}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 579 },u: { $set: { num: 7 } }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1093,donor: 'donor0',recipient: 'recipient0',slot: 3,num: 13}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1094,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 8},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1095,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 2},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 45},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 14},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 47},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 14},u: {donor: 'donor2',recipient: 'recipient0',slot: 8,num: 3}}]}},
        ],
    },
    { // 140
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1096,donor: 'donor1',recipient: 'recipient0',slot: 3,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1097,donor: 'donor2',recipient: 'recipient0',slot: 9,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1098,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1099,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1100,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1101,donor: 'donor0',recipient: 'recipient0',slot: 6,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1102,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1103,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1104,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1105,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 15}]}},
        ],
    },
    { // 141
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1106,donor: 'donor1',recipient: 'recipient0',slot: 25,num: 18},
            {_id: 1107,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 18},
            {_id: 1108,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 4},
            {_id: 1109,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 20},
            {_id: 1110,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 17},
            {_id: 1111,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 2},
            {_id: 1112,donor: 'donor2',recipient: 'recipient0',slot: 29,num: 7},
            {_id: 1113,donor: 'donor0',recipient: 'recipient0',slot: 41,num: 18},
            {_id: 1114,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 1},
            {_id: 1115,donor: 'donor1',recipient: 'recipient0',slot: 40,num: 14},
        ],
    },
    { // 142
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1116,donor: 'donor0',recipient: 'recipient0',slot: 19,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1117,donor: 'donor2',recipient: 'recipient0',slot: 45,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1118,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1119,donor: 'donor0',recipient: 'recipient0',slot: 8,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1120,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1121,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1122,donor: 'donor1',recipient: 'recipient0',slot: 40,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1123,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1124,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1125,donor: 'donor0',recipient: 'recipient0',slot: 35,num: 10}]}},
        ],
    },
    { // 143
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1126,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 4},
            {_id: 1127,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 15},
            {_id: 1128,donor: 'donor1',recipient: 'recipient0',slot: 16,num: 10},
            {_id: 1129,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 19},
            {_id: 1130,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 3},
            {_id: 1131,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 1},
            {_id: 1132,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 1},
            {_id: 1133,donor: 'donor1',recipient: 'recipient0',slot: 3,num: 11},
            {_id: 1134,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 11},
            {_id: 1135,donor: 'donor2',recipient: 'recipient0',slot: 13,num: 1},
        ],
    },
    { // 144
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1136,donor: 'donor2',recipient: 'recipient0',slot: 47,num: 15},
            {_id: 1137,donor: 'donor0',recipient: 'recipient0',slot: 30,num: 18},
            {_id: 1138,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 20},
            {_id: 1139,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 2},
            {_id: 1140,donor: 'donor2',recipient: 'recipient0',slot: 23,num: 3},
            {_id: 1141,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 3},
            {_id: 1142,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 17},
            {_id: 1143,donor: 'donor0',recipient: 'recipient0',slot: 29,num: 6},
            {_id: 1144,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 16},
            {_id: 1145,donor: 'donor0',recipient: 'recipient0',slot: 18,num: 12},
        ],
    },
    { // 145
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1146,donor: 'donor1',recipient: 'recipient0',slot: 3,num: 10},
            {_id: 1147,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 4},
            {_id: 1148,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 10},
            {_id: 1149,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 3},
            {_id: 1150,donor: 'donor0',recipient: 'recipient0',slot: 12,num: 17},
            {_id: 1151,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 9},
            {_id: 1152,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 3},
            {_id: 1153,donor: 'donor2',recipient: 'recipient0',slot: 18,num: 16},
            {_id: 1154,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 10},
            {_id: 1155,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 14},
        ],
    },
    { // 146
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 27},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 529 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 119 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 27},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 21},update: [{ $set: { donor: 'donor2' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1156,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 15}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 459 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 18},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1157,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 18}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1158,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 19},update: { $inc: { num: 0 } },new: true,upsert: true}},
        ],
    },
    { // 147
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1159,donor: 'donor1',recipient: 'recipient0',slot: 41,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1160,donor: 'donor1',recipient: 'recipient0',slot: 36,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1161,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1162,donor: 'donor1',recipient: 'recipient0',slot: 23,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1163,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1164,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1165,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1166,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1167,donor: 'donor1',recipient: 'recipient0',slot: 35,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1168,donor: 'donor1',recipient: 'recipient0',slot: 8,num: 8}]}},
        ],
    },
    { // 148
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1169,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1170,donor: 'donor0',recipient: 'recipient0',slot: 1,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1171,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1172,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1173,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1174,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1175,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1176,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1177,donor: 'donor1',recipient: 'recipient0',slot: 4,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1178,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 2}]}},
        ],
    },
    { // 149
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1179,donor: 'donor1',recipient: 'recipient0',slot: 4,num: 14},
            {_id: 1180,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 14},
            {_id: 1181,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 16},
            {_id: 1182,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 12},
            {_id: 1183,donor: 'donor2',recipient: 'recipient0',slot: 32,num: 18},
            {_id: 1184,donor: 'donor0',recipient: 'recipient0',slot: 25,num: 19},
            {_id: 1185,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 18},
            {_id: 1186,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 2},
            {_id: 1187,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 8},
            {_id: 1188,donor: 'donor1',recipient: 'recipient0',slot: 33,num: 6},
        ],
    },
    { // 150
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1189,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1190,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1191,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1192,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1193,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1194,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1195,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1196,donor: 'donor1',recipient: 'recipient0',slot: 27,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1197,donor: 'donor0',recipient: 'recipient0',slot: 2,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1198,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 3}]}},
        ],
    },
    { // 151
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 43},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 283 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 285 },u: [{ $set: { num: 18 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 757 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 266 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 28},u: {donor: 'donor1',recipient: 'recipient0',slot: 9,num: 16}}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1199,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1200,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1201,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 12}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 144 },limit: 1}]}},
        ],
    },
    { // 152
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 248 },u: [{ $set: { num: 8 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1053 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 15},update: [{ $set: { donor: 'donor1' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 436 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 21},u: { $set: {donor: 'donor1',recipient: 'recipient0'} }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 294 },u: { $set: { num: 9 } }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1202,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 7}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1019 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 49},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 618 },limit: 1}]}},
        ],
    },
    { // 153
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1203,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1204,donor: 'donor2',recipient: 'recipient0',slot: 2,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1205,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1206,donor: 'donor0',recipient: 'recipient0',slot: 30,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1207,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1208,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1209,donor: 'donor0',recipient: 'recipient0',slot: 32,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1210,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1211,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1212,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 13}]}},
        ],
    },
    { // 154
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1213,donor: 'donor1',recipient: 'recipient0',slot: 6,num: 18},
            {_id: 1214,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 10},
            {_id: 1215,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 12},
            {_id: 1216,donor: 'donor1',recipient: 'recipient0',slot: 19,num: 1},
            {_id: 1217,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 12},
            {_id: 1218,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 7},
            {_id: 1219,donor: 'donor1',recipient: 'recipient0',slot: 41,num: 7},
            {_id: 1220,donor: 'donor0',recipient: 'recipient0',slot: 1,num: 11},
            {_id: 1221,donor: 'donor1',recipient: 'recipient0',slot: 8,num: 10},
            {_id: 1222,donor: 'donor0',recipient: 'recipient0',slot: 0,num: 2},
        ],
    },
    { // 155
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 200 },u: [{ $set: { num: 2 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1223,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 10},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 539 },u: { $set: { num: 12 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 362 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 20},u: {donor: 'donor0',recipient: 'recipient0',slot: 6,num: 6}}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 27},u: [{ $set: { donor: 'donor0' } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 537 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 20},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 94 },u: { $set: { num: 13 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 42},u: {donor: 'donor0',recipient: 'recipient0',slot: 12,num: 15}}]}},
        ],
    },
    { // 156
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1224,donor: 'donor2',recipient: 'recipient0',slot: 4,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1225,donor: 'donor1',recipient: 'recipient0',slot: 38,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1226,donor: 'donor1',recipient: 'recipient0',slot: 25,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1227,donor: 'donor2',recipient: 'recipient0',slot: 38,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1228,donor: 'donor1',recipient: 'recipient0',slot: 41,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1229,donor: 'donor1',recipient: 'recipient0',slot: 20,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1230,donor: 'donor2',recipient: 'recipient0',slot: 39,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1231,donor: 'donor0',recipient: 'recipient0',slot: 33,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1232,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1233,donor: 'donor1',recipient: 'recipient0',slot: 6,num: 13}]}},
        ],
    },
    { // 157
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1234,donor: 'donor1',recipient: 'recipient0',slot: 33,num: 3},
            {_id: 1235,donor: 'donor0',recipient: 'recipient0',slot: 38,num: 11},
            {_id: 1236,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 18},
            {_id: 1237,donor: 'donor1',recipient: 'recipient0',slot: 12,num: 11},
            {_id: 1238,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 11},
            {_id: 1239,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 16},
            {_id: 1240,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 5},
            {_id: 1241,donor: 'donor1',recipient: 'recipient0',slot: 47,num: 4},
            {_id: 1242,donor: 'donor0',recipient: 'recipient0',slot: 6,num: 7},
            {_id: 1243,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 19},
        ],
    },
    { // 158
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1244,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 4},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 707 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 18},update: {$set: { donor: 'donor0' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 896 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1245,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 9}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 930 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 11},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 681 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 1},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 33},remove: true}},
        ],
    },
    { // 159
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1246,donor: 'donor2',recipient: 'recipient0',slot: 19,num: 2},
            {_id: 1247,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 6},
            {_id: 1248,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 7},
            {_id: 1249,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 17},
            {_id: 1250,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 18},
            {_id: 1251,donor: 'donor2',recipient: 'recipient0',slot: 21,num: 7},
            {_id: 1252,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 20},
            {_id: 1253,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 1},
            {_id: 1254,donor: 'donor1',recipient: 'recipient0',slot: 9,num: 17},
            {_id: 1255,donor: 'donor2',recipient: 'recipient0',slot: 14,num: 9},
        ],
    },
    { // 160
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1256,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1257,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1258,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1259,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1260,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1261,donor: 'donor1',recipient: 'recipient0',slot: 27,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1262,donor: 'donor1',recipient: 'recipient0',slot: 12,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1263,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1264,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1265,donor: 'donor2',recipient: 'recipient0',slot: 23,num: 19}]}},
        ],
    },
    { // 161
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1266,donor: 'donor2',recipient: 'recipient0',slot: 22,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1267,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1268,donor: 'donor0',recipient: 'recipient0',slot: 6,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1269,donor: 'donor2',recipient: 'recipient0',slot: 18,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1270,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1271,donor: 'donor0',recipient: 'recipient0',slot: 29,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1272,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1273,donor: 'donor2',recipient: 'recipient0',slot: 13,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1274,donor: 'donor2',recipient: 'recipient0',slot: 12,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1275,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 9}]}},
        ],
    },
    { // 162
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1276,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1277,donor: 'donor0',recipient: 'recipient0',slot: 2,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1278,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1279,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1280,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1281,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1282,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1283,donor: 'donor0',recipient: 'recipient0',slot: 4,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1284,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1285,donor: 'donor1',recipient: 'recipient0',slot: 25,num: 4}]}},
        ],
    },
    { // 163
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1286,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 17},
            {_id: 1287,donor: 'donor0',recipient: 'recipient0',slot: 3,num: 8},
            {_id: 1288,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 12},
            {_id: 1289,donor: 'donor1',recipient: 'recipient0',slot: 23,num: 2},
            {_id: 1290,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 18},
            {_id: 1291,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 6},
            {_id: 1292,donor: 'donor0',recipient: 'recipient0',slot: 21,num: 5},
            {_id: 1293,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 14},
            {_id: 1294,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 7},
            {_id: 1295,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 12},
        ],
    },
    { // 164
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1296,donor: 'donor0',recipient: 'recipient0',slot: 38,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1297,donor: 'donor0',recipient: 'recipient0',slot: 32,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1298,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1299,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1300,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1301,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1302,donor: 'donor1',recipient: 'recipient0',slot: 18,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1303,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1304,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1305,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 1}]}},
        ],
    },
    { // 165
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1306,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 14},
            {_id: 1307,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 19},
            {_id: 1308,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 16},
            {_id: 1309,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 15},
            {_id: 1310,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 9},
            {_id: 1311,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 3},
            {_id: 1312,donor: 'donor1',recipient: 'recipient0',slot: 40,num: 16},
            {_id: 1313,donor: 'donor2',recipient: 'recipient0',slot: 21,num: 8},
            {_id: 1314,donor: 'donor1',recipient: 'recipient0',slot: 24,num: 15},
            {_id: 1315,donor: 'donor1',recipient: 'recipient0',slot: 32,num: 10},
        ],
    },
    { // 166
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 19},update: {$set: { donor: 'donor1' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 682 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 37},u: {donor: 'donor2',recipient: 'recipient0',slot: 46,num: 13}}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 24},u: { $set: { recipient: 'recipient0' } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 47},u: [{ $set: { num: 15 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1316,donor: 'donor2',recipient: 'recipient0',slot: 17,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1317,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 9}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 492 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 0},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 303 },limit: 1}]}},
        ],
    },
    { // 167
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 38},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 14},u: [{ $set: { num: 7 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 281 },u: { $set: { num: 1 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 377 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 18},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1318,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 12}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 356 },u: { $set: { num: 3 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 282 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 709 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 9},remove: true}},
        ],
    },
    { // 168
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1319,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 18},
            {_id: 1320,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 1},
            {_id: 1321,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 12},
            {_id: 1322,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 20},
            {_id: 1323,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 2},
            {_id: 1324,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 7},
            {_id: 1325,donor: 'donor2',recipient: 'recipient0',slot: 6,num: 13},
            {_id: 1326,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 13},
            {_id: 1327,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 9},
            {_id: 1328,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 16},
        ],
    },
    { // 169
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1329,donor: 'donor0',recipient: 'recipient0',slot: 4,num: 14},
            {_id: 1330,donor: 'donor0',recipient: 'recipient0',slot: 32,num: 9},
            {_id: 1331,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 4},
            {_id: 1332,donor: 'donor1',recipient: 'recipient0',slot: 47,num: 16},
            {_id: 1333,donor: 'donor2',recipient: 'recipient0',slot: 29,num: 5},
            {_id: 1334,donor: 'donor1',recipient: 'recipient0',slot: 38,num: 4},
            {_id: 1335,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 12},
            {_id: 1336,donor: 'donor0',recipient: 'recipient0',slot: 8,num: 13},
            {_id: 1337,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 20},
            {_id: 1338,donor: 'donor0',recipient: 'recipient0',slot: 32,num: 3},
        ],
    },
    { // 170
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 24},update: [{ $set: { donor: 'donor1' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 625 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1339,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1340,donor: 'donor1',recipient: 'recipient0',slot: 12,num: 6}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1111 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1202 },u: [{ $set: { num: 4 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 379 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1341,donor: 'donor0',recipient: 'recipient0',slot: 45,num: 6}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 135 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 217 },u: [{ $set: { num: 4 } }]}]}},
        ],
    },
    { // 171
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1342,donor: 'donor1',recipient: 'recipient0',slot: 31,num: 15}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 2},u: { $set: {donor: 'donor1',recipient: 'recipient0'} }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1343,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 11}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1344,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 20},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1345,donor: 'donor2',recipient: 'recipient0',slot: 25,num: 10}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 415 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1346,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1347,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 13}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 0},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 834 },limit: 1}]}},
        ],
    },
    { // 172
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 47},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 1},update: [{ $set: { recipient: 'recipient0' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 362 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1124 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1348,donor: 'donor0',recipient: 'recipient0',slot: 34,num: 6},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 21},u: { $set: { donor: 'donor0' } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 34},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 960 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1349,donor: 'donor2',recipient: 'recipient0',slot: 8,num: 7}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1077 },limit: 1}]}},
        ],
    },
    { // 173
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1350,donor: 'donor0',recipient: 'recipient0',slot: 21,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1351,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1352,donor: 'donor0',recipient: 'recipient0',slot: 43,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1353,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1354,donor: 'donor0',recipient: 'recipient0',slot: 9,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1355,donor: 'donor0',recipient: 'recipient0',slot: 21,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1356,donor: 'donor2',recipient: 'recipient0',slot: 41,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1357,donor: 'donor2',recipient: 'recipient0',slot: 20,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1358,donor: 'donor1',recipient: 'recipient0',slot: 36,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1359,donor: 'donor0',recipient: 'recipient0',slot: 45,num: 11}]}},
        ],
    },
    { // 174
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1360,donor: 'donor1',recipient: 'recipient0',slot: 44,num: 6},
            {_id: 1361,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 20},
            {_id: 1362,donor: 'donor0',recipient: 'recipient0',slot: 18,num: 12},
            {_id: 1363,donor: 'donor1',recipient: 'recipient0',slot: 23,num: 12},
            {_id: 1364,donor: 'donor2',recipient: 'recipient0',slot: 15,num: 11},
            {_id: 1365,donor: 'donor2',recipient: 'recipient0',slot: 13,num: 17},
            {_id: 1366,donor: 'donor0',recipient: 'recipient0',slot: 35,num: 17},
            {_id: 1367,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 8},
            {_id: 1368,donor: 'donor2',recipient: 'recipient0',slot: 31,num: 3},
            {_id: 1369,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 18},
        ],
    },
    { // 175
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1370,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1371,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1372,donor: 'donor0',recipient: 'recipient0',slot: 36,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1373,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1374,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1375,donor: 'donor0',recipient: 'recipient0',slot: 42,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1376,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1377,donor: 'donor1',recipient: 'recipient0',slot: 19,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1378,donor: 'donor1',recipient: 'recipient0',slot: 12,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1379,donor: 'donor2',recipient: 'recipient0',slot: 46,num: 4}]}},
        ],
    },
    { // 176
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1380,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1381,donor: 'donor2',recipient: 'recipient0',slot: 15,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1382,donor: 'donor1',recipient: 'recipient0',slot: 12,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1383,donor: 'donor0',recipient: 'recipient0',slot: 19,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1384,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1385,donor: 'donor2',recipient: 'recipient0',slot: 45,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1386,donor: 'donor1',recipient: 'recipient0',slot: 16,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1387,donor: 'donor1',recipient: 'recipient0',slot: 35,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1388,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1389,donor: 'donor1',recipient: 'recipient0',slot: 12,num: 11}]}},
        ],
    },
    { // 177
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 785 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 383 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1390,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 5}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 9 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1391,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 13}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 46},u: [{ $set: { donor: 'donor2' } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 453 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1392,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 3}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 1},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1393,donor: 'donor1',recipient: 'recipient0',slot: 23,num: 19}]}},
        ],
    },
    { // 178
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1394,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1395,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1396,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1397,donor: 'donor1',recipient: 'recipient0',slot: 5,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1398,donor: 'donor2',recipient: 'recipient0',slot: 15,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1399,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1400,donor: 'donor1',recipient: 'recipient0',slot: 30,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1401,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1402,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1403,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 15}]}},
        ],
    },
    { // 179
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1404,donor: 'donor0',recipient: 'recipient0',slot: 0,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1405,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1406,donor: 'donor1',recipient: 'recipient0',slot: 8,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1407,donor: 'donor1',recipient: 'recipient0',slot: 28,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1408,donor: 'donor2',recipient: 'recipient0',slot: 46,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1409,donor: 'donor0',recipient: 'recipient0',slot: 10,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1410,donor: 'donor0',recipient: 'recipient0',slot: 30,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1411,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1412,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1413,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 5}]}},
        ],
    },
    { // 180
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 757 },u: [{ $set: { num: 9 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1414,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 13},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 49},u: {donor: 'donor1',recipient: 'recipient0',slot: 33,num: 13}}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1415,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1416,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 20}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 203 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 97 },u: { $set: { num: 12 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 12},u: { $set: { recipient: 'recipient0' } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 36},u: {donor: 'donor1',recipient: 'recipient0',slot: 17,num: 3}}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 38},u: [{ $set: { num: 5 } }]}]}},
        ],
    },
    { // 181
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1417,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 9}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1418,donor: 'donor0',recipient: 'recipient0',slot: 10,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1419,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1420,donor: 'donor0',recipient: 'recipient0',slot: 36,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1421,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1422,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1423,donor: 'donor1',recipient: 'recipient0',slot: 25,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1424,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1425,donor: 'donor1',recipient: 'recipient0',slot: 15,num: 14}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1426,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 2}]}},
        ],
    },
    { // 182
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1427,donor: 'donor0',recipient: 'recipient0',slot: 16,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1428,donor: 'donor2',recipient: 'recipient0',slot: 21,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1429,donor: 'donor1',recipient: 'recipient0',slot: 19,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1430,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1431,donor: 'donor1',recipient: 'recipient0',slot: 29,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1432,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1433,donor: 'donor0',recipient: 'recipient0',slot: 13,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1434,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1435,donor: 'donor1',recipient: 'recipient0',slot: 41,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1436,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 15}]}},
        ],
    },
    { // 183
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1315 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 26},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 27},update: {$set: {donor: 'donor0',recipient: 'recipient0'},$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1437,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 5}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 32},u: {donor: 'donor2',recipient: 'recipient0',slot: 9,num: 12}}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 228 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 20},u: [{ $set: {donor: 'donor1',recipient: 'recipient0'} }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 47},u: [{ $set: { num: 20 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 628 },u: { $set: { num: 7 } }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1438,donor: 'donor0',recipient: 'recipient0',slot: 34,num: 9}]}},
        ],
    },
    { // 184
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1439,donor: 'donor0',recipient: 'recipient0',slot: 42,num: 17},
            {_id: 1440,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 9},
            {_id: 1441,donor: 'donor2',recipient: 'recipient0',slot: 39,num: 7},
            {_id: 1442,donor: 'donor0',recipient: 'recipient0',slot: 11,num: 19},
            {_id: 1443,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 15},
            {_id: 1444,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 6},
            {_id: 1445,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 8},
            {_id: 1446,donor: 'donor1',recipient: 'recipient0',slot: 24,num: 20},
            {_id: 1447,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 4},
            {_id: 1448,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 3},
        ],
    },
    { // 185
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',recipient: 'recipient0',slot: 44},u: {donor: 'donor2',recipient: 'recipient0',slot: 13,num: 17}}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1252 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 0},update: {$set: { donor: 'donor0' },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1449,donor: 'donor1',recipient: 'recipient0',slot: 33,num: 6}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 573 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 10},update: [{ $set: { num: 9 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 31},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1450,donor: 'donor1',recipient: 'recipient0',slot: 42,num: 5}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 1},u: { $set: { donor: 'donor2' } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 10},u: {donor: 'donor2',recipient: 'recipient0',slot: 9,num: 4}}]}},
        ],
    },
    { // 186
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1451,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 15},
            {_id: 1452,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 10},
            {_id: 1453,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 6},
            {_id: 1454,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 20},
            {_id: 1455,donor: 'donor0',recipient: 'recipient0',slot: 12,num: 18},
            {_id: 1456,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 15},
            {_id: 1457,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 2},
            {_id: 1458,donor: 'donor0',recipient: 'recipient0',slot: 34,num: 18},
            {_id: 1459,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 18},
            {_id: 1460,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 13},
        ],
    },
    { // 187
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1461,donor: 'donor0',recipient: 'recipient0',slot: 21,num: 17}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1013 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1462,donor: 'donor1',recipient: 'recipient0',slot: 17,num: 1},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 34},u: { $set: { recipient: 'recipient0' } }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1463,donor: 'donor1',recipient: 'recipient0',slot: 16,num: 18}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 35},update: [{ $set: { recipient: 'recipient0' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 42},u: [{ $set: { donor: 'donor2' } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 400 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 0},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1464,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 10}]}},
        ],
    },
    { // 188
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1465,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1466,donor: 'donor2',recipient: 'recipient0',slot: 29,num: 13}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1467,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1468,donor: 'donor1',recipient: 'recipient0',slot: 18,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1469,donor: 'donor2',recipient: 'recipient0',slot: 32,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1470,donor: 'donor0',recipient: 'recipient0',slot: 2,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1471,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 4}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1472,donor: 'donor0',recipient: 'recipient0',slot: 6,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1473,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1474,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 6}]}},
        ],
    },
    { // 189
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1475,donor: 'donor2',recipient: 'recipient0',slot: 31,num: 18},
            {_id: 1476,donor: 'donor0',recipient: 'recipient0',slot: 35,num: 11},
            {_id: 1477,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 8},
            {_id: 1478,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 6},
            {_id: 1479,donor: 'donor0',recipient: 'recipient0',slot: 49,num: 12},
            {_id: 1480,donor: 'donor1',recipient: 'recipient0',slot: 16,num: 2},
            {_id: 1481,donor: 'donor0',recipient: 'recipient0',slot: 19,num: 1},
            {_id: 1482,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 18},
            {_id: 1483,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 4},
            {_id: 1484,donor: 'donor1',recipient: 'recipient0',slot: 48,num: 8},
        ],
    },
    { // 190
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1485,donor: 'donor0',recipient: 'recipient0',slot: 3,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1486,donor: 'donor1',recipient: 'recipient0',slot: 3,num: 18}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1487,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1488,donor: 'donor2',recipient: 'recipient0',slot: 13,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1489,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1490,donor: 'donor1',recipient: 'recipient0',slot: 5,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1491,donor: 'donor1',recipient: 'recipient0',slot: 36,num: 20}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1492,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 12}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1493,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 5}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1494,donor: 'donor1',recipient: 'recipient0',slot: 47,num: 3}]}},
        ],
    },
    { // 191
        type: 'retryableInsert',
        phase: 'seed',
        documents: [
            {_id: 1495,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 18},
            {_id: 1496,donor: 'donor1',recipient: 'recipient0',slot: 2,num: 1},
            {_id: 1497,donor: 'donor0',recipient: 'recipient0',slot: 14,num: 15},
            {_id: 1498,donor: 'donor2',recipient: 'recipient0',slot: 38,num: 19},
            {_id: 1499,donor: 'donor2',recipient: 'recipient0',slot: 6,num: 20},
            {_id: 1500,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 18},
            {_id: 1501,donor: 'donor0',recipient: 'recipient0',slot: 39,num: 5},
            {_id: 1502,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 19},
            {_id: 1503,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 19},
            {_id: 1504,donor: 'donor0',recipient: 'recipient0',slot: 47,num: 6},
        ],
    },
    { // 192
        type: 'plain',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1505,donor: 'donor2',recipient: 'recipient0',slot: 15,num: 17}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1506,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 6}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1507,donor: 'donor1',recipient: 'recipient0',slot: 32,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1508,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1509,donor: 'donor1',recipient: 'recipient0',slot: 35,num: 3}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1510,donor: 'donor0',recipient: 'recipient0',slot: 34,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1511,donor: 'donor2',recipient: 'recipient0',slot: 9,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1512,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1513,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1514,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 16}]}},
        ],
    },
    { // 193
        type: 'runTransaction',
        phase: 'seed',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1515,donor: 'donor0',recipient: 'recipient0',slot: 21,num: 10}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1111 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 13},u: {donor: 'donor1',recipient: 'recipient0',slot: 9,num: 6}}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 325 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1516,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 13}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 151 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1517,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 15}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 669 },u: [{ $set: { num: 12 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1358 },u: [{ $set: { num: 12 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 43},u: [{ $set: {donor: 'donor1',recipient: 'recipient0'} }]}]}},
        ],
    },
    { // 194
        type: 'retryableDelete',
        phase: 'resharding',
        deletes: [
            {q: { _id: 828 },limit: 1},
            {q: { _id: 270 },limit: 1},
            {q: { _id: 876 },limit: 1},
            {q: { _id: 192 },limit: 1},
            {q: { _id: 694 },limit: 1},
            {q: { _id: 251 },limit: 1},
            {q: { _id: 33 },limit: 1},
            {q: { _id: 850 },limit: 1},
            {q: { _id: 668 },limit: 1},
            {q: { _id: 175 },limit: 1},
        ],
    },
    { // 195
        type: 'retryableUpdate',
        phase: 'resharding',
        updates: [
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 41},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 5},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 16},u: {donor: 'donor0',recipient: 'recipient0',slot: 1,num: 3}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 39},u: { $set: { donor: 'donor2' } }},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 46},u: {donor: 'donor1',recipient: 'recipient0',slot: 31,num: 15}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 25},u: {donor: 'donor2',recipient: 'recipient0',slot: 26,num: 3}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 46},u: { $set: { num: 9 } }},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 49},u: { $set: {donor: 'donor0',recipient: 'recipient0'} }},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 13},u: { $set: {donor: 'donor2',recipient: 'recipient0'} }},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 43},u: [{ $set: { donor: 'donor1' } }]},
        ],
    },
    { // 196
        type: 'stepup',
        phase: 'resharding',
        role: 'donor',
        shard: 0,
    },
    { // 197
        type: 'stepup',
        phase: 'resharding',
        role: 'donor',
        shard: 2,
    },
    { // 198
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 216 },u: { $set: { num: 11 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 894 },u: [{ $set: { num: 14 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 17},u: [{ $set: { num: 13 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1518,donor: 'donor0',recipient: 'recipient0',slot: 29,num: 11}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 203 },u: [{ $set: { num: 18 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1519,donor: 'donor0',recipient: 'recipient0',slot: 17,num: 11}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1348 },u: [{ $set: { num: 14 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 0},u: { $set: { num: 12 } }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1520,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 2}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1012 },limit: 1}]}},
        ],
    },
    { // 199
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 14},u: { $set: {donor: 'donor2',recipient: 'recipient0'} }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1521,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 16}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1502 },u: { $set: { num: 7 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1522,donor: 'donor2',recipient: 'recipient0',slot: 13,num: 11},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 242 },u: { $set: { num: 11 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 8},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 35},u: { $set: { num: 11 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1480 },u: { $set: { num: 9 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 48},u: { $set: { num: 19 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 220 },limit: 1}]}},
        ],
    },
    { // 200
        type: 'stepup',
        phase: 'resharding',
        role: 'donor',
        shard: 0,
    },
    { // 201
        type: 'retryableUpdate',
        phase: 'resharding',
        updates: [
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 33},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 30},u: [{ $set: {donor: 'donor2',recipient: 'recipient0'} }]},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 1},u: { $set: {donor: 'donor2',recipient: 'recipient0'} }},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 24},u: { $set: { recipient: 'recipient0' } }},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 25},u: {donor: 'donor0',recipient: 'recipient0',slot: 48,num: 7}},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 12},u: {donor: 'donor1',recipient: 'recipient0',slot: 48,num: 4}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 8},u: { $set: { num: 13 } }},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 36},u: [{ $set: { donor: 'donor2' } }]},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 21},u: [{ $set: {donor: 'donor0',recipient: 'recipient0'} }]},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 10},u: {donor: 'donor1',recipient: 'recipient0',slot: 22,num: 17}},
        ],
    },
    { // 202
        type: 'retryableUpdate',
        phase: 'resharding',
        updates: [
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 26},u: {donor: 'donor0',recipient: 'recipient0',slot: 31,num: 11}},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 15},u: [{ $set: { donor: 'donor0' } }]},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 19},u: [{ $set: { num: 4 } }]},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 29},u: { $set: { donor: 'donor2' } }},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 8},u: {donor: 'donor0',recipient: 'recipient0',slot: 28,num: 4}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 40},u: { $set: { recipient: 'recipient0' } }},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 35},u: [{ $set: { donor: 'donor0' } }]},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 45},u: {donor: 'donor0',recipient: 'recipient0',slot: 33,num: 1}},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 41},u: {donor: 'donor0',recipient: 'recipient0',slot: 10,num: 11}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 11},u: { $set: { recipient: 'recipient0' } }},
        ],
    },
    { // 203
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1361 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1523,donor: 'donor0',recipient: 'recipient0',slot: 26,num: 10}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 518 },u: { $set: { num: 12 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1210 },u: { $set: { num: 13 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 35},u: { $set: {donor: 'donor2',recipient: 'recipient0'} }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 22},u: { $set: { recipient: 'recipient0' } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 9},u: { $set: { num: 9 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 571 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 22},u: [{ $set: { num: 14 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 476 },u: [{ $set: { num: 2 } }]}]}},
        ],
    },
    { // 204
        type: 'retryableInsert',
        phase: 'resharding',
        documents: [
            {_id: 1524,donor: 'donor1',recipient: 'recipient0',slot: 4,num: 17},
            {_id: 1525,donor: 'donor1',recipient: 'recipient0',slot: 38,num: 7},
            {_id: 1526,donor: 'donor0',recipient: 'recipient0',slot: 42,num: 17},
            {_id: 1527,donor: 'donor0',recipient: 'recipient0',slot: 25,num: 20},
            {_id: 1528,donor: 'donor2',recipient: 'recipient0',slot: 11,num: 15},
            {_id: 1529,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 1},
            {_id: 1530,donor: 'donor1',recipient: 'recipient0',slot: 8,num: 5},
            {_id: 1531,donor: 'donor0',recipient: 'recipient0',slot: 46,num: 17},
            {_id: 1532,donor: 'donor1',recipient: 'recipient0',slot: 44,num: 5},
            {_id: 1533,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 20},
        ],
    },
    { // 205
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1534,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 20}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 12},u: {donor: 'donor1',recipient: 'recipient0',slot: 24,num: 15}}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 48},u: [{ $set: { num: 1 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1535,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 17}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1536,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 2},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1537,donor: 'donor2',recipient: 'recipient0',slot: 39,num: 13}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 27},u: { $set: { num: 8 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 7},u: { $set: { recipient: 'recipient0' } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1538,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 17},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1421 },u: [{ $set: { num: 19 } }]}]}},
        ],
    },
    { // 206
        type: 'retryableDelete',
        phase: 'resharding',
        deletes: [
            {q: { _id: 672 },limit: 1},
            {q: { _id: 271 },limit: 1},
            {q: { _id: 539 },limit: 1},
            {q: { _id: 249 },limit: 1},
            {q: { _id: 1114 },limit: 1},
            {q: { _id: 252 },limit: 1},
            {q: { _id: 937 },limit: 1},
            {q: { _id: 911 },limit: 1},
            {q: { _id: 1072 },limit: 1},
            {q: { _id: 1176 },limit: 1},
        ],
    },
    { // 207
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 2},u: { $set: { num: 7 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1539,donor: 'donor2',recipient: 'recipient0',slot: 17,num: 6},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 752 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 963 },u: [{ $set: { num: 10 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1540,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 9}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 6},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 65 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 8},u: [{ $set: { donor: 'donor2' } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1541,donor: 'donor2',recipient: 'recipient0',slot: 17,num: 5}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 41},remove: true}},
        ],
    },
    { // 208
        type: 'retryableDelete',
        phase: 'resharding',
        deletes: [
            {q: { _id: 963 },limit: 1},
            {q: { _id: 459 },limit: 1},
            {q: { _id: 438 },limit: 1},
            {q: { _id: 1497 },limit: 1},
            {q: { _id: 1006 },limit: 1},
            {q: { _id: 760 },limit: 1},
            {q: { _id: 1002 },limit: 1},
            {q: { _id: 375 },limit: 1},
            {q: { _id: 1529 },limit: 1},
            {q: { _id: 82 },limit: 1},
        ],
    },
    { // 209
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1253 },u: { $set: { num: 18 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 939 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 105 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 75 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1429 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',recipient: 'recipient0',slot: 34},u: [{ $set: { num: 13 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 36},update: {$set: {donor: 'donor0',recipient: 'recipient0'},$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1542,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 4}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 1},update: [{ $set: { recipient: 'recipient0' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 10},u: {donor: 'donor2',recipient: 'recipient0',slot: 24,num: 19}}]}},
        ],
    },
    { // 210
        type: 'retryableUpdate',
        phase: 'resharding',
        updates: [
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 15},u: { $set: { num: 20 } }},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 45},u: [{ $set: { num: 1 } }]},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 44},u: { $set: { recipient: 'recipient0' } }},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 24},u: [{ $set: { num: 2 } }]},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 1},u: {donor: 'donor1',recipient: 'recipient0',slot: 3,num: 13}},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 44},u: { $set: { num: 2 } }},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 9},u: {donor: 'donor0',recipient: 'recipient0',slot: 43,num: 14}},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 39},u: [{ $set: { donor: 'donor0' } }]},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 36},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 42},u: [{ $set: { donor: 'donor0' } }]},
        ],
    },
    { // 211
        type: 'retryableUpdate',
        phase: 'resharding',
        updates: [
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 18},u: { $set: { donor: 'donor0' } }},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 16},u: {donor: 'donor0',recipient: 'recipient0',slot: 33,num: 18}},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 28},u: { $set: {donor: 'donor0',recipient: 'recipient0'} }},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 2},u: { $set: { recipient: 'recipient0' } }},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 1},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 12},u: [{ $set: {donor: 'donor0',recipient: 'recipient0'} }]},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 5},u: { $set: { num: 12 } }},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 14},u: {donor: 'donor1',recipient: 'recipient0',slot: 0,num: 17}},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 28},u: { $set: {donor: 'donor1',recipient: 'recipient0'} }},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 35},u: [{ $set: {donor: 'donor1',recipient: 'recipient0'} }]},
        ],
    },
    { // 212
        type: 'retryableFindAndModify',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1543,donor: 'donor2',recipient: 'recipient0',slot: 19,num: 8},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1544,donor: 'donor2',recipient: 'recipient0',slot: 21,num: 19},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 32},update: {$set: { donor: 'donor1' },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 33},update: [{ $set: { num: 8 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1545,donor: 'donor1',recipient: 'recipient0',slot: 11,num: 9},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 11},update: [{ $set: { donor: 'donor1' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 7},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1546,donor: 'donor0',recipient: 'recipient0',slot: 40,num: 4},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 4},update: {$set: { donor: 'donor1' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1547,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 6},update: { $inc: { num: 0 } },new: true,upsert: true}},
        ],
    },
    { // 213
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1072 },u: [{ $set: { num: 19 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1548,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 15}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1549,donor: 'donor0',recipient: 'recipient0',slot: 32,num: 16}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1053 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1550,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 8}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1551,donor: 'donor2',recipient: 'recipient0',slot: 8,num: 3}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1552,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 12},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 987 },u: { $set: { num: 10 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1092 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 32},u: [{ $set: { donor: 'donor2' } }]}]}},
        ],
    },
    { // 214
        type: 'retryableFindAndModify',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 19},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1553,donor: 'donor1',recipient: 'recipient0',slot: 18,num: 6},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1554,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 5},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 6},update: {$set: { num: 3 },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1555,donor: 'donor1',recipient: 'recipient0',slot: 28,num: 2},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 49},update: [{ $set: { num: 11 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 42},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 49},update: {$set: { num: 9 },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1556,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 14},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1557,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 3},update: { $inc: { num: 0 } },new: true,upsert: true}},
        ],
    },
    { // 215
        type: 'stepup',
        phase: 'resharding',
        role: 'donor',
        shard: 0,
    },
    { // 216
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1558,donor: 'donor2',recipient: 'recipient0',slot: 32,num: 20},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 419 },u: { $set: { num: 16 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1559,donor: 'donor1',recipient: 'recipient0',slot: 41,num: 20},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 33},u: { $set: { num: 1 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 141 },u: { $set: { num: 4 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 29},update: {$set: {donor: 'donor1',recipient: 'recipient0'},$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1560,donor: 'donor0',recipient: 'recipient0',slot: 30,num: 2}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1561,donor: 'donor0',recipient: 'recipient0',slot: 19,num: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1562,donor: 'donor2',recipient: 'recipient0',slot: 46,num: 10}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 915 },limit: 1}]}},
        ],
    },
    { // 217
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 592 },u: { $set: { num: 8 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 328 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1563,donor: 'donor0',recipient: 'recipient0',slot: 44,num: 20}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1124 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 19},u: [{ $set: { num: 3 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 80 },u: { $set: { num: 16 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 22},u: [{ $set: { num: 19 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 33},u: [{ $set: { donor: 'donor0' } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1254 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 905 },limit: 1}]}},
        ],
    },
    { // 218
        type: 'retryableInsert',
        phase: 'resharding',
        documents: [
            {_id: 1564,donor: 'donor0',recipient: 'recipient0',slot: 32,num: 19},
            {_id: 1565,donor: 'donor0',recipient: 'recipient0',slot: 31,num: 5},
            {_id: 1566,donor: 'donor1',recipient: 'recipient0',slot: 10,num: 12},
            {_id: 1567,donor: 'donor1',recipient: 'recipient0',slot: 3,num: 5},
            {_id: 1568,donor: 'donor2',recipient: 'recipient0',slot: 35,num: 20},
            {_id: 1569,donor: 'donor2',recipient: 'recipient0',slot: 23,num: 16},
            {_id: 1570,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 10},
            {_id: 1571,donor: 'donor0',recipient: 'recipient0',slot: 22,num: 20},
            {_id: 1572,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 4},
            {_id: 1573,donor: 'donor2',recipient: 'recipient0',slot: 31,num: 15},
        ],
    },
    { // 219
        type: 'retryableFindAndModify',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 47},update: [{ $set: {donor: 'donor2',recipient: 'recipient0'} },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 48},update: {$set: { num: 15 },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1574,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 1},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1575,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 16},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1576,donor: 'donor0',recipient: 'recipient0',slot: 37,num: 16},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 25},update: [{ $set: { num: 18 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1577,donor: 'donor0',recipient: 'recipient0',slot: 10,num: 5},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1578,donor: 'donor1',recipient: 'recipient0',slot: 30,num: 6},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 36},update: [{ $set: { num: 9 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 41},update: [{ $set: { donor: 'donor0' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
        ],
    },
    { // 220
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 23},u: [{ $set: { num: 13 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 31},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 958 },u: { $set: { num: 14 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 540 },u: [{ $set: { num: 16 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 47},u: [{ $set: { donor: 'donor1' } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 772 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1579,donor: 'donor0',recipient: 'recipient0',slot: 27,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1580,donor: 'donor1',recipient: 'recipient0',slot: 5,num: 18}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1581,donor: 'donor0',recipient: 'recipient0',slot: 10,num: 12},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 20},u: { $set: { num: 11 } }}]}},
        ],
    },
    { // 221
        type: 'stepup',
        phase: 'resharding',
        role: 'donor',
        shard: 1,
    },
    { // 222
        type: 'retryableInsert',
        phase: 'resharding',
        documents: [
            {_id: 1582,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 19},
            {_id: 1583,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 8},
            {_id: 1584,donor: 'donor2',recipient: 'recipient0',slot: 8,num: 11},
            {_id: 1585,donor: 'donor1',recipient: 'recipient0',slot: 24,num: 17},
            {_id: 1586,donor: 'donor2',recipient: 'recipient0',slot: 15,num: 2},
            {_id: 1587,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 1},
            {_id: 1588,donor: 'donor2',recipient: 'recipient0',slot: 23,num: 13},
            {_id: 1589,donor: 'donor1',recipient: 'recipient0',slot: 20,num: 9},
            {_id: 1590,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 15},
            {_id: 1591,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 10},
        ],
    },
    { // 223
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1592,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 20}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1529 },u: [{ $set: { num: 10 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1593,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 17}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 736 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 15},remove: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1305 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 46},u: [{ $set: { num: 4 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1386 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1161 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 40},remove: true}},
        ],
    },
    { // 224
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 28 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 39},remove: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1594,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 19}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 25},u: { $set: { num: 8 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 540 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 40},update: {$set: { num: 13 },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1272 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1123 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1595,donor: 'donor1',recipient: 'recipient0',slot: 1,num: 14}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1437 },limit: 1}]}},
        ],
    },
    { // 225
        type: 'retryableInsert',
        phase: 'resharding',
        documents: [
            {_id: 1596,donor: 'donor0',recipient: 'recipient0',slot: 30,num: 3},
            {_id: 1597,donor: 'donor1',recipient: 'recipient0',slot: 0,num: 6},
            {_id: 1598,donor: 'donor1',recipient: 'recipient0',slot: 49,num: 17},
            {_id: 1599,donor: 'donor1',recipient: 'recipient0',slot: 30,num: 6},
            {_id: 1600,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 4},
            {_id: 1601,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 13},
            {_id: 1602,donor: 'donor1',recipient: 'recipient0',slot: 40,num: 14},
            {_id: 1603,donor: 'donor1',recipient: 'recipient0',slot: 21,num: 5},
            {_id: 1604,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 20},
            {_id: 1605,donor: 'donor2',recipient: 'recipient0',slot: 31,num: 3},
        ],
    },
    { // 226
        type: 'retryableDelete',
        phase: 'resharding',
        deletes: [
            {q: { _id: 385 },limit: 1},
            {q: { _id: 40 },limit: 1},
            {q: { _id: 1119 },limit: 1},
            {q: { _id: 1520 },limit: 1},
            {q: { _id: 1206 },limit: 1},
            {q: { _id: 1038 },limit: 1},
            {q: { _id: 907 },limit: 1},
            {q: { _id: 383 },limit: 1},
            {q: { _id: 268 },limit: 1},
            {q: { _id: 401 },limit: 1},
        ],
    },
    { // 227
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 825 },u: [{ $set: { num: 14 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 965 },u: { $set: { num: 13 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 188 },u: { $set: { num: 9 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 635 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1606,donor: 'donor1',recipient: 'recipient0',slot: 4,num: 8}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 7},u: [{ $set: {donor: 'donor0',recipient: 'recipient0'} }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1400 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1383 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 33},u: {donor: 'donor1',recipient: 'recipient0',slot: 35,num: 7}}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 746 },limit: 1}]}},
        ],
    },
    { // 228
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 0},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 43},remove: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 32},u: { $set: { num: 12 } }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1607,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 19}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 49},u: { $set: { donor: 'donor1' } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 7},u: { $set: { num: 3 } }}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 2},update: [{ $set: { recipient: 'recipient0' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1608,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 18}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1534 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1335 },u: [{ $set: { num: 13 } }]}]}},
        ],
    },
    { // 229
        type: 'retryableInsert',
        phase: 'resharding',
        documents: [
            {_id: 1609,donor: 'donor1',recipient: 'recipient0',slot: 34,num: 18},
            {_id: 1610,donor: 'donor2',recipient: 'recipient0',slot: 5,num: 17},
            {_id: 1611,donor: 'donor1',recipient: 'recipient0',slot: 41,num: 18},
            {_id: 1612,donor: 'donor2',recipient: 'recipient0',slot: 28,num: 7},
            {_id: 1613,donor: 'donor2',recipient: 'recipient0',slot: 44,num: 9},
            {_id: 1614,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 17},
            {_id: 1615,donor: 'donor0',recipient: 'recipient0',slot: 36,num: 2},
            {_id: 1616,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 6},
            {_id: 1617,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 17},
            {_id: 1618,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 20},
        ],
    },
    { // 230
        type: 'retryableInsert',
        phase: 'resharding',
        documents: [
            {_id: 1619,donor: 'donor1',recipient: 'recipient0',slot: 6,num: 20},
            {_id: 1620,donor: 'donor1',recipient: 'recipient0',slot: 16,num: 14},
            {_id: 1621,donor: 'donor2',recipient: 'recipient0',slot: 12,num: 10},
            {_id: 1622,donor: 'donor1',recipient: 'recipient0',slot: 25,num: 6},
            {_id: 1623,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 9},
            {_id: 1624,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 6},
            {_id: 1625,donor: 'donor2',recipient: 'recipient0',slot: 45,num: 16},
            {_id: 1626,donor: 'donor2',recipient: 'recipient0',slot: 25,num: 12},
            {_id: 1627,donor: 'donor0',recipient: 'recipient0',slot: 48,num: 8},
            {_id: 1628,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 11},
        ],
    },
    { // 231
        type: 'retryableDelete',
        phase: 'resharding',
        deletes: [
            {q: { _id: 1019 },limit: 1},
            {q: { _id: 736 },limit: 1},
            {q: { _id: 351 },limit: 1},
            {q: { _id: 883 },limit: 1},
            {q: { _id: 1154 },limit: 1},
            {q: { _id: 90 },limit: 1},
            {q: { _id: 44 },limit: 1},
            {q: { _id: 127 },limit: 1},
            {q: { _id: 134 },limit: 1},
            {q: { _id: 1116 },limit: 1},
        ],
    },
    { // 232
        type: 'retryableFindAndModify',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1629,donor: 'donor0',recipient: 'recipient0',slot: 7,num: 5},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1630,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 20},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 42},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1631,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 3},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1632,donor: 'donor0',recipient: 'recipient0',slot: 4,num: 14},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 0},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 36},update: [{ $set: { recipient: 'recipient0' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 26},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1633,donor: 'donor1',recipient: 'recipient0',slot: 19,num: 6},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1634,donor: 'donor2',recipient: 'recipient0',slot: 12,num: 8},update: { $inc: { num: 0 } },new: true,upsert: true}},
        ],
    },
    { // 233
        type: 'retryableFindAndModify',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 0},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 31},update: {$set: { donor: 'donor1' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1635,donor: 'donor0',recipient: 'recipient0',slot: 24,num: 9},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1636,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 8},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1637,donor: 'donor0',recipient: 'recipient0',slot: 43,num: 14},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 24},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1638,donor: 'donor2',recipient: 'recipient0',slot: 42,num: 8},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 8},update: [{ $set: {donor: 'donor0',recipient: 'recipient0'} },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 41},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1639,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 7},update: { $inc: { num: 0 } },new: true,upsert: true}},
        ],
    },
    { // 234
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 642 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 32},u: [{ $set: { num: 6 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 14},u: {donor: 'donor0',recipient: 'recipient0',slot: 14,num: 2}}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 432 },u: { $set: { num: 1 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1133 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 719 },u: { $set: { num: 17 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 419 },u: [{ $set: { num: 12 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1640,donor: 'donor1',recipient: 'recipient0',slot: 46,num: 2},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1641,donor: 'donor1',recipient: 'recipient0',slot: 13,num: 20}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1360 },u: { $set: { num: 10 } }}]}},
        ],
    },
    { // 235
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 507 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',slot: 26},update: [{ $set: { num: 3 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: false}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1642,donor: 'donor0',recipient: 'recipient0',slot: 4,num: 14}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 18},u: {donor: 'donor1',recipient: 'recipient0',slot: 15,num: 3}}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 306 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 285 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 5},update: [{ $set: { recipient: 'recipient0' } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 49},u: [{ $set: { recipient: 'recipient0' } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1643,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 8},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 49},u: { $set: { num: 1 } }}]}},
        ],
    },
    { // 236
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1644,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 9}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1366 },u: [{ $set: { num: 12 } }]}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1145 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1645,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 15}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 236 },u: { $set: { num: 14 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 249 },u: [{ $set: { num: 17 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1646,donor: 'donor1',recipient: 'recipient0',slot: 4,num: 5}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 6},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 27},update: [{ $set: { num: 7 } },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1647,donor: 'donor0',recipient: 'recipient0',slot: 41,num: 20}]}},
        ],
    },
    { // 237
        type: 'stepup',
        phase: 'resharding',
        role: 'donor',
        shard: 0,
    },
    { // 238
        type: 'retryableUpdate',
        phase: 'resharding',
        updates: [
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 19},u: {donor: 'donor1',recipient: 'recipient0',slot: 8,num: 9}},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 43},u: { $set: { recipient: 'recipient0' } }},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 20},u: { $set: { num: 13 } }},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 24},u: {donor: 'donor1',recipient: 'recipient0',slot: 28,num: 14}},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 39},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 27},u: {donor: 'donor0',recipient: 'recipient0',slot: 24,num: 11}},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 2},u: { $set: { recipient: 'recipient0' } }},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 35},u: { $set: { num: 2 } }},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 20},u: [{ $set: { donor: 'donor0' } }]},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 19},u: { $set: { num: 5 } }},
        ],
    },
    { // 239
        type: 'retryableInsert',
        phase: 'resharding',
        documents: [
            {_id: 1648,donor: 'donor2',recipient: 'recipient0',slot: 48,num: 8},
            {_id: 1649,donor: 'donor2',recipient: 'recipient0',slot: 22,num: 12},
            {_id: 1650,donor: 'donor2',recipient: 'recipient0',slot: 7,num: 20},
            {_id: 1651,donor: 'donor1',recipient: 'recipient0',slot: 44,num: 20},
            {_id: 1652,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 19},
            {_id: 1653,donor: 'donor0',recipient: 'recipient0',slot: 38,num: 5},
            {_id: 1654,donor: 'donor0',recipient: 'recipient0',slot: 15,num: 2},
            {_id: 1655,donor: 'donor2',recipient: 'recipient0',slot: 16,num: 17},
            {_id: 1656,donor: 'donor2',recipient: 'recipient0',slot: 26,num: 13},
            {_id: 1657,donor: 'donor2',recipient: 'recipient0',slot: 43,num: 17},
        ],
    },
    { // 240
        type: 'retryableInsert',
        phase: 'resharding',
        documents: [
            {_id: 1658,donor: 'donor2',recipient: 'recipient0',slot: 21,num: 1},
            {_id: 1659,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 12},
            {_id: 1660,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 9},
            {_id: 1661,donor: 'donor1',recipient: 'recipient0',slot: 23,num: 11},
            {_id: 1662,donor: 'donor1',recipient: 'recipient0',slot: 43,num: 18},
            {_id: 1663,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 11},
            {_id: 1664,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 18},
            {_id: 1665,donor: 'donor1',recipient: 'recipient0',slot: 9,num: 1},
            {_id: 1666,donor: 'donor2',recipient: 'recipient0',slot: 14,num: 20},
            {_id: 1667,donor: 'donor1',recipient: 'recipient0',slot: 45,num: 5},
        ],
    },
    { // 241
        type: 'retryableFindAndModify',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1668,donor: 'donor1',recipient: 'recipient0',slot: 27,num: 12},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 45},update: {$set: {donor: 'donor0',recipient: 'recipient0'},$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 24},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 7},update: {$set: { donor: 'donor2' },$inc: { dummy: 1 }},new: false}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1669,donor: 'donor1',recipient: 'recipient0',slot: 44,num: 12},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 6},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1670,donor: 'donor2',recipient: 'recipient0',slot: 14,num: 14},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1671,donor: 'donor1',recipient: 'recipient0',slot: 39,num: 1},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1672,donor: 'donor1',recipient: 'recipient0',slot: 3,num: 3},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 9},update: {$set: { num: 20 },$inc: { dummy: 1 }},new: true}},
        ],
    },
    { // 242
        type: 'stepup',
        phase: 'resharding',
        role: 'donor',
        shard: 1,
    },
    { // 243
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1673,donor: 'donor0',recipient: 'recipient0',slot: 42,num: 14}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1674,donor: 'donor2',recipient: 'recipient0',slot: 49,num: 3},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 37},u: [{ $set: { num: 12 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1675,donor: 'donor1',recipient: 'recipient0',slot: 20,num: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 795 },u: [{ $set: { num: 20 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1676,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 16}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1677,donor: 'donor2',recipient: 'recipient0',slot: 27,num: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1383 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1678,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 19}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',slot: 1},update: {$set: { recipient: 'recipient0' },$inc: { dummy: 1 }},new: false}},
        ],
    },
    { // 244
        type: 'retryableFindAndModify',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1679,donor: 'donor1',recipient: 'recipient0',slot: 22,num: 18},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1680,donor: 'donor2',recipient: 'recipient0',slot: 30,num: 20},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1681,donor: 'donor1',recipient: 'recipient0',slot: 37,num: 19},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1682,donor: 'donor1',recipient: 'recipient0',slot: 28,num: 12},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 15},update: {$set: { donor: 'donor2' },$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 9},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1683,donor: 'donor0',recipient: 'recipient0',slot: 23,num: 5},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 48},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor2',recipient: 'recipient0',slot: 42},remove: true}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',recipient: 'recipient0',slot: 3},remove: true}},
        ],
    },
    { // 245
        type: 'retryableUpdate',
        phase: 'resharding',
        updates: [
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 23},u: [{ $set: {donor: 'donor0',recipient: 'recipient0'} }]},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 37},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 21},u: {donor: 'donor1',recipient: 'recipient0',slot: 27,num: 18}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 28},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 24},u: { $set: { donor: 'donor2' } }},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 27},u: {donor: 'donor2',recipient: 'recipient0',slot: 30,num: 12}},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 37},u: { $set: {donor: 'donor1',recipient: 'recipient0'} }},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 12},u: {donor: 'donor1',recipient: 'recipient0',slot: 11,num: 2}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 45},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 40},u: {donor: 'donor0',recipient: 'recipient0',slot: 9,num: 3}},
        ],
    },
    { // 246
        type: 'retryableUpdate',
        phase: 'resharding',
        updates: [
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 26},u: { $set: {donor: 'donor0',recipient: 'recipient0'} }},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 42},u: {donor: 'donor1',recipient: 'recipient0',slot: 29,num: 11}},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 36},u: {donor: 'donor1',recipient: 'recipient0',slot: 44,num: 7}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 49},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 16},u: {donor: 'donor1',recipient: 'recipient0',slot: 14,num: 19}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 2},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 6},u: [{ $set: {donor: 'donor2',recipient: 'recipient0'} }]},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 8},u: {donor: 'donor1',recipient: 'recipient0',slot: 24,num: 13}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 49},u: [{ $set: { num: 9 } }]},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 1},u: [{ $set: { donor: 'donor1' } }]},
        ],
    },
    { // 247
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 763 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor0',recipient: 'recipient0',slot: 39},update: [{ $set: {donor: 'donor0',recipient: 'recipient0'} },{ $set: { dummy: { $add: [{ $ifNull: ['$dummy',0] },1] } } }],new: true}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 394 },u: [{ $set: { num: 18 } }]}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {_id: 1684,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 9},update: { $inc: { num: 0 } },new: true,upsert: true}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1449 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1685,donor: 'donor0',recipient: 'recipient0',slot: 28,num: 19}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1686,donor: 'donor2',recipient: 'recipient0',slot: 25,num: 11}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1687,donor: 'donor0',recipient: 'recipient0',slot: 28,num: 10}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1688,donor: 'donor2',recipient: 'recipient0',slot: 46,num: 5}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1142 },u: [{ $set: { num: 10 } }]}]}},
        ],
    },
    { // 248
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor2',slot: 40},u: { $set: { num: 8 } }}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 670 },limit: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 2},u: [{ $set: { recipient: 'recipient0' } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1689,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 7}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1634 },limit: 1}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1690,donor: 'donor0',recipient: 'recipient0',slot: 20,num: 1}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 314 },u: { $set: { num: 6 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 1459 },u: [{ $set: { num: 1 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor0',slot: 17},u: [{ $set: { num: 12 } }]}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1691,donor: 'donor2',recipient: 'recipient0',slot: 36,num: 18}]}},
        ],
    },
    { // 249
        type: 'retryableInsert',
        phase: 'resharding',
        documents: [
            {_id: 1692,donor: 'donor1',recipient: 'recipient0',slot: 26,num: 14},
            {_id: 1693,donor: 'donor1',recipient: 'recipient0',slot: 41,num: 12},
            {_id: 1694,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 5},
            {_id: 1695,donor: 'donor1',recipient: 'recipient0',slot: 7,num: 18},
            {_id: 1696,donor: 'donor0',recipient: 'recipient0',slot: 3,num: 20},
            {_id: 1697,donor: 'donor0',recipient: 'recipient0',slot: 12,num: 10},
            {_id: 1698,donor: 'donor2',recipient: 'recipient0',slot: 34,num: 13},
            {_id: 1699,donor: 'donor0',recipient: 'recipient0',slot: 5,num: 5},
            {_id: 1700,donor: 'donor2',recipient: 'recipient0',slot: 23,num: 8},
            {_id: 1701,donor: 'donor1',recipient: 'recipient0',slot: 47,num: 8},
        ],
    },
    { // 250
        type: 'plain',
        phase: 'resharding',
        ops: [
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 308 },limit: 1}]}},
            {dbName: kDbName,commandObj: {delete: kCollName,deletes: [{q: { _id: 1619 },limit: 1}]}},
            {dbName: kDbName,commandObj: {findAndModify: kCollName,query: {donor: 'donor1',slot: 4},update: {$set: {donor: 'donor2',recipient: 'recipient0'},$inc: { dummy: 1 }},new: true}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1702,donor: 'donor0',recipient: 'recipient0',slot: 10,num: 7}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1703,donor: 'donor1',recipient: 'recipient0',slot: 18,num: 16}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 6},u: { $set: { num: 17 } }}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 872 },u: [{ $set: { num: 10 } }]}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: {donor: 'donor1',slot: 5},u: { $set: { num: 3 } }}]}},
            {dbName: kDbName,commandObj: {insert: kCollName,documents: [{_id: 1704,donor: 'donor0',recipient: 'recipient0',slot: 0,num: 4}]}},
            {dbName: kDbName,commandObj: {update: kCollName,updates: [{q: { _id: 38 },u: [{ $set: { num: 19 } }]}]}},
        ],
    },
    { // 251
        type: 'stepup',
        phase: 'resharding',
        role: 'recipient',
        shard: 0,
    },
    { // 252
        type: 'retryableUpdate',
        phase: 'resharding',
        updates: [
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 37},u: { $set: {donor: 'donor1',recipient: 'recipient0'} }},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 0},u: {donor: 'donor2',recipient: 'recipient0',slot: 7,num: 20}},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 9},u: [{ $set: { recipient: 'recipient0' } }]},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 28},u: { $set: { num: 19 } }},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 40},u: { $set: { recipient: 'recipient0' } }},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 7},u: [{ $set: { donor: 'donor1' } }]},
            {q: {donor: 'donor0',recipient: 'recipient0',slot: 30},u: { $set: { num: 9 } }},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 20},u: {donor: 'donor2',recipient: 'recipient0',slot: 28,num: 17}},
            {q: {donor: 'donor1',recipient: 'recipient0',slot: 29},u: { $set: {donor: 'donor2',recipient: 'recipient0'} }},
            {q: {donor: 'donor2',recipient: 'recipient0',slot: 10},u: {donor: 'donor1',recipient: 'recipient0',slot: 28,num: 2}},
        ],
    },
    { // 253
        type: 'retryableInsert',
        phase: 'resharding',
        documents: [
            {_id: 1705,donor: 'donor1',recipient: 'recipient0',slot: 16,num: 14},
            {_id: 1706,donor: 'donor1',recipient: 'recipient0',slot: 35,num: 7},
            {_id: 1707,donor: 'donor2',recipient: 'recipient0',slot: 37,num: 5},
            {_id: 1708,donor: 'donor0',recipient: 'recipient0',slot: 8,num: 8},
            {_id: 1709,donor: 'donor2',recipient: 'recipient0',slot: 33,num: 4},
            {_id: 1710,donor: 'donor2',recipient: 'recipient0',slot: 24,num: 13},
            {_id: 1711,donor: 'donor2',recipient: 'recipient0',slot: 10,num: 17},
            {_id: 1712,donor: 'donor2',recipient: 'recipient0',slot: 1,num: 17},
            {_id: 1713,donor: 'donor0',recipient: 'recipient0',slot: 0,num: 3},
            {_id: 1714,donor: 'donor0',recipient: 'recipient0',slot: 2,num: 14},
        ],
    },
    { // 254
        type: 'retryableDelete',
        phase: 'resharding',
        deletes: [
            {q: { _id: 1273 },limit: 1},
            {q: { _id: 267 },limit: 1},
            {q: { _id: 16 },limit: 1},
            {q: { _id: 1232 },limit: 1},
            {q: { _id: 1698 },limit: 1},
            {q: { _id: 1225 },limit: 1},
            {q: { _id: 846 },limit: 1},
            {q: { _id: 1436 },limit: 1},
            {q: { _id: 32 },limit: 1},
            {q: { _id: 212 },limit: 1},
        ],
    },
    { // 255
        type: 'retryableDelete',
        phase: 'resharding',
        deletes: [
            {q: { _id: 104 },limit: 1},
            {q: { _id: 1491 },limit: 1},
            {q: { _id: 1390 },limit: 1},
            {q: { _id: 674 },limit: 1},
            {q: { _id: 797 },limit: 1},
            {q: { _id: 349 },limit: 1},
            {q: { _id: 83 },limit: 1},
            {q: { _id: 1296 },limit: 1},
            {q: { _id: 1518 },limit: 1},
            {q: { _id: 773 },limit: 1},
        ],
    },
    { // 256
        type: 'retryableInsert',
        phase: 'resharding',
        documents: [
            {_id: 1715,donor: 'donor2',recipient: 'recipient0',slot: 3,num: 8},
            {_id: 1716,donor: 'donor0',recipient: 'recipient0',slot: 35,num: 6},
            {_id: 1717,donor: 'donor1',recipient: 'recipient0',slot: 30,num: 7},
            {_id: 1718,donor: 'donor1',recipient: 'recipient0',slot: 24,num: 11},
            {_id: 1719,donor: 'donor0',recipient: 'recipient0',slot: 2,num: 16},
            {_id: 1720,donor: 'donor0',recipient: 'recipient0',slot: 17,num: 14},
            {_id: 1721,donor: 'donor2',recipient: 'recipient0',slot: 9,num: 16},
            {_id: 1722,donor: 'donor2',recipient: 'recipient0',slot: 40,num: 15},
            {_id: 1723,donor: 'donor1',recipient: 'recipient0',slot: 14,num: 16},
            {_id: 1724,donor: 'donor2',recipient: 'recipient0',slot: 0,num: 6},
        ],
    },
];

// `phaseChange` is the index of the first command in the `steps` array run after the resharding
// operation has started.

const phaseChange = 194;

const shell = globalThis;


/**
 * Log a string to output.
 *
 * The log destination and an optional prefix are configurable by calling `log.setup`.
 *
 * The `log` variable is a singleton. If you `require` this module in multiple other modules,
 * the setup parameters are retained. In this way you can call `log.setup({prefix: 'my fuzzer '})`
 * at the beginning of your `main` function and all subsequent uses of `log('foo')` will have the
 * prefix prepended.
 *
 * @param {any} msg
 */
const log = function(msg) {
    log.printer(`${log.prefix}${msg}`);
};

/**
 * @param prefix prefix to add before every log call. You probably want a space at the end of your prefix.
 * @param printer callback that takes the `prefix + msg` as a parameter.
 */
log.setup = function({prefix, printer} = {prefix: ''}) {
    if (typeof prefix !== 'undefined') {
        log.prefix = prefix;
    }
    if (typeof printer !== 'undefined') {
        log.printer = printer;
    }
};

/**
 * Reset the `log` singleton to its original state.
 *
 * The prefix is cleared, and the `printer` callback (from calls to `log.setup()`) is reverted
 * to either `shell.print` or `console.log` depending on the environment.
 */
log.reset = function() {
    log.prefix = '';
    log.printer =
        typeof shell !== 'undefined' && typeof shell.print !== 'undefined'
            ? shell.print
            : Function.prototype;
};

// Reset prior to exposing log to set the prefix and printer initial values.
log.reset();

const shell$1 = globalThis;
const kStartedState = 'started';
const kPreparedState = 'prepared';
const kCommittedState = 'committed';
const kAbortedState = 'aborted';
const kNoDecisionYet = undefined;
const kCommitDecision = 'commit';
const kAbortDecision = 'abort';
const kAbortFailedTransactionDbName = 'db_for_unconditional_abort_txn';
const kAbortFailedTransactionCollName = 'coll_for_unconditional_abort_txn';
/**
 * Namespace used by TransactionManager when conditionally aborting failed transactions by starting
 * a new transaction on the session.
 */
const kAbortFailedTransactionNs = `${kAbortFailedTransactionDbName}.${kAbortFailedTransactionCollName}`;
class TransactionManager {
    constructor({ primary, sessionOptions, journalPreparedTxns, fuzzerName }) {
        this._primary = primary;
        this._sessionOptions = sessionOptions;
        this._journalPreparedTxns = journalPreparedTxns;
        this._fuzzerName = fuzzerName;
        this._txns = [];
        this._sessions = [];
        this._stats = {
            startTransaction: 0,
            commitTransaction: 0,
            abortTransaction: 0,
            prepareTransaction: 0,
            skippedTransaction: 0,
        };
        this._subsequentOpsWillBeRolledBack = false;
    }
    static get kInitialSync() {
        return 'initial sync';
    }
    static get kRollback() {
        return 'rollback';
    }
    static get kResharding() {
        return 'resharding';
    }
    stats() {
        return this._stats;
    }
    static _newTransaction(session, startedCommandIndex, canPrepareTransaction) {
        return {
            state: kStartedState,
            session,
            startedCommandIndex,
            canPrepareTransaction,
            preparedCommandIndex: undefined,
            prepareTimestamp: undefined,
            coordinatorDecision: kNoDecisionYet,
            majorityCommittedState: undefined,
        };
    }
    hasOutstandingTransactions() {
        return this._txns.some(txnInfo => txnInfo.state === kStartedState || txnInfo.state === kPreparedState);
    }
    _beginTransaction(ops, commandIndex) {
        if (ops.length === 0) {
            // It is an error to have the prepareTransaction, commitTransaction, or
            // abortTransaction commands be the first command in a multi-statement transaction.
            // Ideally the grammar would never generate an empty 'ops' array, but for now we
            // skip these commands explicitly.
            log(`Skipping empty transaction of cmd ${commandIndex}`);
            this._stats.skippedTransaction++;
            return { session: null, transactionCreatesCollection: false };
        }
        let transactionCreatesCollection = false;
        const session = this._primary.startSession(this._sessionOptions);
        this._sessions.push(session);
        for (const { dbName, commandObj } of ops) {
            const collName = commandObj[Object.keys(commandObj)[0]];
            const cmdRes = session
                .getClient()
                .getDB(dbName)
                .runCommand({
                listCollections: 1,
                nameOnly: true,
                filter: { name: collName },
            });
            shell$1.assert.commandWorked(cmdRes);
            shell$1.assert.neq(undefined, cmdRes.cursor);
            if (cmdRes.cursor.firstBatch.length === 0) {
                transactionCreatesCollection = true;
            }
        }
        session.startTransaction();
        this._stats.startTransaction++;
        let cmdRes;
        let shouldAbort = false;
        for (const { dbName, commandObj } of ops) {
            cmdRes = session.getDatabase(dbName).runCommand(commandObj);
            if (cmdRes.ok !== 1 || cmdRes.hasOwnProperty('writeErrors')) {
                shouldAbort = true;
                break;
            }
            shell$1.assert.commandWorked(cmdRes);
        }
        if (shouldAbort) {
            this.abortFailedTransaction(session);
            log(`Completed transaction of cmd ${commandIndex} with lsid` +
                ` ${shell$1.tojson(session.getSessionId())}. Aborted due to an error:` +
                ` ${shell$1.tojson(cmdRes)}`);
            session.endSession();
            return { session: null, transactionCreatesCollection: false };
        }
        return { session, transactionCreatesCollection };
    }
    abortFailedTransaction(session) {
        const abortRes = session.abortTransaction_forTesting();
        if (abortRes.ok === 1) {
            shell$1.assert.commandWorked(abortRes);
        }
        else {
            // The transaction may have been implicitly aborted by the server.
            shell$1.assert.commandFailedWithCode(abortRes, shell$1.ErrorCodes.NoSuchTransaction);
        }
        ++this._stats.abortTransaction;
        // Suppose the startTransaction=true command is the one which failed. Due to a lack of
        // strict message ordering, shard A may process the startTransaction=true command (which had
        // failed on shard B) after it processed the abortTransaction command. Shard A would then
        // start the transaction despite both mongos and the client having already attempted to
        // abort it. This leads to an idle transaction having been "leaked" on one of the shards.
        // To guarantee the resources from the failed transaction are always cleaned up, we run a
        // dummy aggregation that targets all shards using a higher txnNumber with the same logical
        // session. The higher txnNumber unconditionally causes the lower txnNumber transaction to
        // abort.
        session.startTransaction();
        ++this._stats.startTransaction;
        const aggRes = session.getDatabase(kAbortFailedTransactionDbName).runCommand({
            aggregate: kAbortFailedTransactionCollName,
            pipeline: [],
            cursor: { batchSize: 0 },
        });
        shell$1.assert.commandWorked(aggRes);
        const commitRes = session.commitTransaction_forTesting();
        shell$1.assert.commandWorked(commitRes);
        ++this._stats.commitTransaction;
    }
    runTransaction(ops, commandIndex) {
        const beginRes = this._beginTransaction(ops, commandIndex);
        const session = beginRes['session'];
        if (session === null) {
            return;
        }
        shell$1.assert.commandWorked(session.commitTransaction_forTesting());
        log(`Committed transaction of cmd ${commandIndex}.` +
            ` LSID: ${shell$1.tojson(session.getSessionId())}` +
            ` COMMAND: ${shell$1.tojsononeline(ops)}`);
        this._stats.commitTransaction++;
        session.endSession();
    }
    startTransaction(ops, commandIndex) {
        const beginRes = this._beginTransaction(ops, commandIndex);
        const session = beginRes['session'];
        if (session === null) {
            return;
        }
        let canPrepareTransaction = true;
        for (const { dbName, commandObj } of ops) {
            const collName = commandObj[Object.keys(commandObj)[0]];
            const cmdRes = session
                .getClient()
                .getDB(dbName)
                .runCommand({
                collStats: collName,
            });
            // The collection must exist for us to have successfully run operations in a
            // transaction against it.
            shell$1.assert.commandWorked(cmdRes);
            if (cmdRes.capped) {
                canPrepareTransaction = false;
                break;
            }
        }
        canPrepareTransaction = canPrepareTransaction
            ? beginRes['transactionCreatesCollection']
            : canPrepareTransaction;
        log(`Started transaction of cmd ${commandIndex} with lsid` +
            ` ${shell$1.tojson(session.getSessionId())}. It` +
            ` ${canPrepareTransaction ? 'does' : 'does not'} involve non-preparable operations` +
            ` and therefore ${canPrepareTransaction ? 'cannot' : 'can'} be prepared.`);
        this._txns.push(TransactionManager._newTransaction(session, commandIndex, canPrepareTransaction));
    }
    static _canPrepareTransaction(txnInfo) {
        // Attempting to run a multi-statement transaction on a capped collection in a sharded
        // cluster returns an error. We skip running the prepareTransaction command in this case
        // in order to continue exercising the behavior of multi-statement transactions on a
        // capped collection in a replica set.
        return txnInfo.state === kStartedState && !txnInfo.canPrepareTransaction;
    }
    static _prepareTransaction(primary, txnInfo, journalPreparedTxns, commandIndex) {
        const commandObj = { prepareTransaction: 1, writeConcern: { j: journalPreparedTxns } };
        const delegatingSession = new shell$1._DelegatingDriverSession(primary, txnInfo.session);
        const res = delegatingSession.getDatabase('admin').runCommand(commandObj);
        shell$1.assert.commandWorked(res);
        txnInfo.state = kPreparedState;
        txnInfo.preparedCommandIndex = commandIndex;
        txnInfo.prepareTimestamp = res.prepareTimestamp;
        log(`Prepared transaction of cmd ${commandIndex} from` +
            ` ${txnInfo.startedCommandIndex} with lsid` +
            ` ${shell$1.tojson(txnInfo.session.getSessionId())} at timestamp` +
            ` ${shell$1.tojson(txnInfo.prepareTimestamp)}.`);
    }
    prepareTransaction(commandIndex) {
        const txnInfo = this._txns.find(TransactionManager._canPrepareTransaction, this);
        if (txnInfo === undefined) {
            log(`Skipping prepareTransaction of cmd ${commandIndex}`);
            return;
        }
        TransactionManager._prepareTransaction(this._primary, txnInfo, this._journalPreparedTxns, commandIndex);
        this._stats.prepareTransaction++;
    }
    _canCommitTransaction(txnInfo) {
        if (txnInfo.state === kStartedState) {
            return true;
        }
        if (txnInfo.state === kPreparedState) {
            // In MongoDB's distributed commit protocol, the coordinator won't run the
            // commitTransaction command until the prepareTransaction operation is
            // majority-committed. The rollback fuzzer doesn't run the prepareTransaction
            // command with w="majority" in order to avoid hanging forever while the primary is
            // connected only to tiebreaker node. (The latter having paused replication.) In
            // order to match the coordinator's behavior, the rollback fuzzer must not attempt
            // to commit a transaction for which both the prepareTransaction and
            // commitTransaction operations would be rolled back. If
            // `this._subsequentOpsWillBeRolledBack === false`, then we are guaranteed that the
            // commitTransaction will eventually become majority-committed.
            if (this._subsequentOpsWillBeRolledBack &&
                txnInfo.majorityCommittedState !== kPreparedState) {
                return false;
            }
            return (txnInfo.coordinatorDecision === kNoDecisionYet ||
                txnInfo.coordinatorDecision === kCommitDecision);
        }
        return false;
    }
    static _commitTransaction(primary, txnInfo, journalPreparedTxns, commandIndex) {
        shell$1.assert.neq(txnInfo.coordinatorDecision, kAbortDecision, 'attempted to commit transaction that was previously attempted to be aborted');
        let msg;
        const commandObj = { commitTransaction: 1 };
        if (txnInfo.state === kPreparedState) {
            txnInfo.coordinatorDecision = kCommitDecision;
            // We use prepare timestamp as the commit timestamp in order to match the behavior
            // of the PrepareHelpers.commitTransaction() helper in
            // jstests/core/txns/libs/prepare_helper.js.
            commandObj.commitTimestamp = txnInfo.prepareTimestamp;
            commandObj.writeConcern = { j: journalPreparedTxns };
            msg =
                `Commit attempt for prepared transaction of cmd ${commandIndex} from` +
                    ` ${txnInfo.preparedCommandIndex} with lsid` +
                    ` ${shell$1.tojson(txnInfo.session.getSessionId())} at timestamp` +
                    ` ${shell$1.tojson(commandObj.commitTimestamp)} `;
        }
        else {
            msg =
                `Commit attempt for  non-prepared transaction of cmd ${commandIndex} from` +
                    ` ${txnInfo.startedCommandIndex} with lsid` +
                    ` ${shell$1.tojson(txnInfo.session.getSessionId())} `;
        }
        const delegatingSession = new shell$1._DelegatingDriverSession(primary, txnInfo.session);
        const res = delegatingSession.getDatabase('admin').runCommand(commandObj);
        if (!res.ok &&
            res.hasOwnProperty('code') &&
            res['code'] === shell$1.ErrorCodes.WriteConflict) {
            msg += 'failed with WriteConflict. Transaction was aborted.';
            txnInfo.state = kAbortedState;
        }
        else {
            shell$1.assert.commandWorked(res);
            msg += 'was successful.';
            txnInfo.state = kCommittedState;
        }
        log(msg);
    }
    commitTransaction(commandIndex) {
        const txnInfo = this._txns.find(this._canCommitTransaction, this);
        if (txnInfo === undefined) {
            log(`Skipping commitTransaction of cmd ${commandIndex}`);
            return;
        }
        TransactionManager._commitTransaction(this._primary, txnInfo, this._journalPreparedTxns, commandIndex);
        if (txnInfo.state === kAbortedState) {
            this._stats.abortTransaction++;
        }
        else {
            this._stats.commitTransaction++;
        }
    }
    static _canAbortTransaction(txnInfo) {
        return (txnInfo.state === kStartedState ||
            (txnInfo.state === kPreparedState &&
                (txnInfo.coordinatorDecision === kNoDecisionYet ||
                    txnInfo.coordinatorDecision === kAbortDecision)));
    }
    static _abortTransaction(primary, txnInfo, journalPreparedTxns, commandIndex) {
        shell$1.assert.neq(txnInfo.coordinatorDecision, kCommitDecision, 'attempted to abort transaction that was previously attempted to be committed');
        let msg;
        const commandObj = { abortTransaction: 1 };
        if (txnInfo.state === kPreparedState) {
            txnInfo.coordinatorDecision = kAbortDecision;
            commandObj.writeConcern = { j: journalPreparedTxns };
            msg =
                `Aborted prepared transaction of cmd ${commandIndex} from` +
                    ` ${txnInfo.preparedCommandIndex} with lsid` +
                    ` ${shell$1.tojson(txnInfo.session.getSessionId())}.`;
        }
        else {
            msg =
                `Aborted non-prepared transaction of cmd ${commandIndex} from` +
                    ` ${txnInfo.startedCommandIndex} with lsid` +
                    ` ${shell$1.tojson(txnInfo.session.getSessionId())}.`;
        }
        const delegatingSession = new shell$1._DelegatingDriverSession(primary, txnInfo.session);
        const res = delegatingSession.getDatabase('admin').runCommand(commandObj);
        shell$1.assert.commandWorked(res);
        txnInfo.state = kAbortedState;
        log(msg);
    }
    abortTransaction(commandIndex) {
        const txnInfo = this._txns.find(TransactionManager._canAbortTransaction, this);
        if (txnInfo === undefined) {
            log(`Skipping abortTransaction of cmd ${commandIndex}`);
            return;
        }
        TransactionManager._abortTransaction(this._primary, txnInfo, this._journalPreparedTxns, commandIndex);
        this._stats.abortTransaction++;
    }
    endSessions() {
        for (const session of this._sessions) {
            session.endSession();
        }
    }
    /**
     * Saves the current state of each transaction as being majority-committed due to how their
     * effect has replicated to the secondary node (if it was prepared).
     *
     * This function is intended to be called immediately after
     * shell.RollbackTest#transitionToRollbackOperations() is called. Any subsequent operations
     * performed against the current primary will therefore be undone as part of rollback.
     */
    markTransactionsAsMajorityCommitted() {
        // This function is only applicable to the rollback fuzzer.
        shell$1.assert.eq(this._fuzzerName, TransactionManager.kRollback);
        shell$1.assert(!this._subsequentOpsWillBeRolledBack, 'expected earlier call to resetTransactionsToMajorityCommittedState()');
        this._subsequentOpsWillBeRolledBack = true;
        // Transactions in the 'kStartedState' are preserved in order to exercise what happens
        // if the prepareTransaction operation is rolled back (possibly in addition to a
        // abortTransaction operation). Note that 'this._primary' hasn't changed and so the
        // server still has all the in-memory state about the transactions.
        this._txns = this._txns.filter(txnInfo => {
            txnInfo.majorityCommittedState = txnInfo.state;
            return txnInfo.state === kStartedState || txnInfo.state === kPreparedState;
        });
    }
    /**
     * Restores the current state of each transaction as the majority-committed state saved in
     * the prior call to markTransactionsAsMajorityCommitted(). Any transaction that wasn't in
     * the prepared state at or before the common point is ignored because no information about
     * it would have been replicated to the secondary (now 'newPrimary') node.
     *
     * This function is intended to be called immediately after
     * shell.RollbackTest#transitionToSyncSourceOperationsBeforeRollback() is called.
     */
    resetTransactionsToMajorityCommittedState(newPrimary) {
        // This function is only applicable to the rollback fuzzer.
        shell$1.assert.eq(this._fuzzerName, TransactionManager.kRollback);
        shell$1.assert(this._subsequentOpsWillBeRolledBack, 'expected earlier call to markTransactionsAsMajorityCommitted()');
        this._primary = newPrimary;
        this._subsequentOpsWillBeRolledBack = false;
        this._txns = this._txns.filter(txnInfo => {
            txnInfo.state = txnInfo.majorityCommittedState;
            return txnInfo.majorityCommittedState === kPreparedState;
        });
    }
    /**
     * Discard the in-memory state of any transactions that are not in the prepared state. Any
     * transaction that wasn't in the prepared state is ignored because no information about it
     * would have been replicated to the secondary (now 'newPrimary') node.
     *
     * This is only intended to be called by the rollback fuzzer.
     *
     * @param newPrimary Node
     * @return void
     */
    discardNonPreparedTransactions(newPrimary) {
        shell$1.assert.eq(this._fuzzerName, TransactionManager.kRollback);
        this._primary = newPrimary;
        // Transactions in the 'kCommittedState' and 'kAbortedState' states are preserved
        // because it is possible for their majority-committed state to be 'kPreparedState' and
        // for resetTransactionsToMajorityCommittedState() to be called later on.
        this._txns = this._txns.filter(txnInfo => txnInfo.state !== kStartedState);
    }
    drainOutstandingTransactions(commandIndex) {
        for (let i = 0; i < this._txns.length; ++i) {
            const txnInfo = this._txns[i];
            if (TransactionManager._canPrepareTransaction(txnInfo) && i % 3 === 0) {
                TransactionManager._prepareTransaction(this._primary, txnInfo, this._journalPreparedTxns, commandIndex);
            }
            if (this._canCommitTransaction(txnInfo) &&
                (txnInfo.coordinatorDecision === kCommitDecision || i % 2 === 0)) {
                TransactionManager._commitTransaction(this._primary, txnInfo, this._journalPreparedTxns, commandIndex);
            }
            else if (TransactionManager._canAbortTransaction(txnInfo)) {
                TransactionManager._abortTransaction(this._primary, txnInfo, this._journalPreparedTxns, commandIndex);
            }
        }
        // Sanity check the post-condition of calling drainOutstandingTransactions(). The
        // earlier for-loop should have aborted the transactions it had decided not to commit
        // that were still active.
        shell$1.assert(!this.hasOutstandingTransactions(), 'drainOutstandingTransactions() left behind outstanding transactions');
    }
}

const knobs = {
    /**
     * TODO: Use the test runtime to determine reasonable values for these settings. 300 comes from
     * the corresponding values for the initial sync fuzzer.
     */
    kNumMaxStepsCountForResharding: 300,
    kNumMaxStepsCountForSeedData: 300,

    /**
     * Making the number of operations in each step too high will cause transactions to be more
     * likely to abort due to write conflicts.
     *
     * TODO: Use the ReshardingStats to determine reasonable values for these settings. 12 comes
     * from corresponding values for the initial sync fuzzer.
     */
    kNumMaxOpsPerStepCountForResharding: 12,
    kNumMaxOpsPerStepCountForSeedData: 12,

    /**
     * The likelihood for whether multi-document transactions are run as part of seeding the initial
     * collection data before the resharding operation is started.
     *
     * If this value is set to 0, then multi-document transactions won't be run during the
     * resharding operation either.
     */
    kAllowTxnProbability: 0.8,

    /**
     * The likelihood for whether retryable writes are run as part of seeding the initial collection
     * data before the resharding operation is started.
     *
     * If this value is set to 0, then retryable writes won't be run during the resharding operation
     * either.
     */
    kAllowRetryableWriteProbability: 0.8,

    /**
     * Used for generating values for the "slot" field with the {donor: 1, slot: 1} and
     * {recipient: 1, slot: 1} shard key patterns.
     *
     * Each shard owns `kNumChunksPerShard` chunks where each chunk contains `kChunkRangeSize`
     * values of "slot":
     *
     *      [MinKey, 10), [10, 20), [20, 30), [30, 40), and [40, MaxKey].
     */
    kNumChunksPerShard: 5,
    kChunkRangeSize: 10,

    /**
     * The likelihood for whether this resharding operation should be same-key resharding. If true,
     * the reshardCollection cmd should use {donor: 1, slot: 1} as new shard key pattern and pass
     * forceRedistribution: true.
     */
    kSameKeyReshardingProbability: 0.5,
};

const shell$2 = globalThis;


class ReshardingStatsImpl {
    constructor() {
        this._commandSuccess = 0;
        this._commandFailure = 0;
        this._commandBreakdown = {};
    }

    _defaultCommandBreakdown(commandName) {
        this._commandBreakdown[commandName] = this._commandBreakdown[commandName] || {
            success: 0,
            failure: 0,
            errorCodes: {},
        };

        return this._commandBreakdown[commandName];
    }

    _recordCommandSuccess(commandName) {
        ++this._commandSuccess;
        ++this._defaultCommandBreakdown(commandName).success;
    }

    _recordCommandFailure(commandName, codeName) {
        ++this._commandFailure;

        const commandStats = this._defaultCommandBreakdown(commandName);
        ++commandStats.failure;

        commandStats.errorCodes[codeName] = commandStats.errorCodes[codeName] || 0;
        ++commandStats.errorCodes[codeName];
    }

    recordEvent(op, res) {
        const commandName = Object.keys(op.commandObj)[0];
        if (res.ok === 1 && !res.writeErrors) {
            this._recordCommandSuccess(commandName);
        } else if (res.ok === 1 && res.writeErrors) {
            for (let writeError of res.writeErrors) {
                // We use shell.ErrorCodeStrings in order to look up the name of a numeric error
                // code. If the error code doesn't correspond to a named error code, then we use the
                // number itself as the name.
                const codeName = shell$2.ErrorCodeStrings.hasOwnProperty(writeError.code)
                    ? shell$2.ErrorCodeStrings[writeError.code]
                    : writeError.code.toString();

                this._recordCommandFailure(commandName, codeName);
            }
        } else {
            this._recordCommandFailure(commandName, res.codeName);
        }
    }

    get raw() {
        return {
            success: this._commandSuccess,
            failure: this._commandFailure,
            breakdown: this._commandBreakdown,
        };
    }
}

class SeedDataStatsImpl {
    constructor() {
        this._insertCount = 0;
        this._commandBreakdown = {};
    }

    /**
     * Returns stats breakdown at arbitrarily nested level, creating the nested structure along the
     * way if necessary.
     */
    _defaultCommandBreakdown(key) {
        this._commandBreakdown[key] = this._commandBreakdown[key] || {insertCount: 0};

        return this._commandBreakdown[key];
    }

    recordDocumentInserted(op) {
        const commandName = Object.keys(op.commandObj)[0];
        const {dbName, commandObj} = op;
        const collName = commandObj[commandName];
        const key = `${dbName}.${collName}`;

        ++this._insertCount;
        ++this._defaultCommandBreakdown(key).insertCount;
    }

    get raw() {
        return {
            insertCount: this._insertCount,
            breakdown: this._commandBreakdown,
        };
    }
}

const shell$3 = globalThis;

/**
 * Component for testing the exactly once semantics of retryable writes in resharding.
 */
class RetryableWriteManager {
    constructor(mongos, 
    // shell.bsonWoCompare() is used by RetryableWriteManager is two different contexts. We
    // dependency inject the function to split up these contexts and make the class easier to
    // unit test.
    compareWriteResults = shell$3.bsonWoCompare, compareTimestamps = shell$3.bsonWoCompare) {
        this.mongos = mongos;
        this.compareWriteResults = compareWriteResults;
        this.compareTimestamps = compareTimestamps;
        this.responses = [];
    }
    /**
     * Runs the insert, update, delete, or findAndModify command as a retryable write.
     *
     * This function returns without an error even if the server returns an ok=0 or write error
     * response. It also isn't an error for `commandObj` to not match or not change a document.
     */
    runCommand(dbName, commandObj, commandIdx) {
        const session = this.mongos.startSession({
            causalConsistency: false,
            retryWrites: false,
        });
        // We manually add the lsid to the command request so it'll also appear in the log message.
        commandObj.lsid = session.getSessionId();
        // runCommand() when using retryWrites=true would assign the txnNumber to a separate copy
        // of the command request. We manually add the txnNumber to the command request so the same
        // txnNumber will be used when the command request is retried in check().
        commandObj.txnNumber = new shell$3.NumberLong(commandIdx);
        const firstRes = session.getDatabase(dbName).runCommand(commandObj);
        if (!this.didWriteForEveryStatement(commandObj, firstRes)) {
            // The exactly once semantics for retryable writes are deceptively weak. This is because
            // retryable writes in MongoDB actually allow each statement to be executed once,
            // independently of whether the other statements in the same command object have already
            // been executed. This means an update or delete command which didn't match a document
            // on its first execution could successfully match and modify a document when retried
            // and still technically be obeying "exactly once" semantics. We exclude such cases for
            // the purposes of checkExactlyOnceSemantics().
            session.endSession();
            return firstRes;
        }
        const secondRes = session.getDatabase(dbName).runCommand(commandObj);
        if (this.wasImplicitlyConvertedToTransaction(commandObj, secondRes)) {
            // An update which modifies the shard key value and causes the document to move owning
            // shards will be executed as a multi-document transaction. A retryable write that has
            // been implicitly converted into a multi-document transaction cannot be retried. We
            // exclude such cases for the purposes of checkExactlyOnceSemantics().
            session.endSession();
            return firstRes;
        }
        this.assertRespondedWithSameWriteResult(commandObj, commandIdx, firstRes, secondRes);
        this.responses.push({ session, dbName, commandObj, commandIdx, result: firstRes });
        return firstRes;
    }
    extractCommandName(commandObj) {
        return Object.keys(commandObj)[0];
    }
    extractWriteResult(res) {
        const { n, nModified, value } = res;
        return { n, nModified, value };
    }
    wasImplicitlyConvertedToTransaction(commandObj, res) {
        const check = errInfo => errInfo.code === shell$3.ErrorCodes.IncompleteTransactionHistory;
        const commandName = this.extractCommandName(commandObj);
        return ((commandName === 'update' &&
            res.writeErrors !== undefined &&
            res.writeErrors.some(writeError => check(writeError))) ||
            (commandName === 'findAndModify' && check(res)));
    }
    didWriteForEveryStatement(commandObj, res) {
        const commandName = this.extractCommandName(commandObj);
        if ('insert' === commandName) {
            return res.n === commandObj.documents.length;
        }
        else if ('update' === commandName) {
            return res.nModified === commandObj.updates.length;
        }
        else if ('delete' === commandName) {
            return res.n === commandObj.deletes.length;
        }
        else if ('findAndModify' === commandName) {
            // `res.value` won't be defined if the findAndModify command returned an error response
            // and `res.value` will be null if the findAndModify command didn't match a document.
            return Boolean(res.value);
        }
        else {
            throw new Error(`Unsupported command: ${commandName}`);
        }
    }
    didRespondWithSameWriteResult(commandObj, commandIdx, firstRes, secondRes) {
        // Update and delete commands may be broadcasted to all shards in the cluster. This can
        // lead to multiple shards reporting 'n' and 'nModified' values for the same statements.
        // As a workaround, we only assert these two values in `secondRes` are at least as large
        // as the ones in `firstRes` rather than asserting that they are equal.
        const commandName = this.extractCommandName(commandObj);
        const compareResult = this.compareWriteResults(this.extractWriteResult(firstRes), this.extractWriteResult(secondRes));
        return commandName === 'update' || commandName === 'delete'
            ? compareResult <= 0
            : compareResult === 0;
    }
    assertRespondedWithSameWriteResult(commandObj, commandIdx, firstRes, secondRes) {
        const errorCtx = { commandObj, commandIdx, firstRes, secondRes };
        shell$3.assert(this.didRespondWithSameWriteResult(commandObj, commandIdx, firstRes, secondRes), Object.assign({ message: 'retryable write generated different response upon retry' }, errorCtx));
    }
    checkExactlyOnceSemantics(cloneTimestamp, skipCheckSameResultForFindAndModify = false) {
        const shardConns = this.establishDirectShardConnections();
        let haveSeenFirstWriteCommittedAfterCloningStarted = false;
        for (const { session, dbName, commandObj, commandIdx, result: firstRes } of this.responses) {
            const writeCommittedBeforeCloningStarted = this.compareTimestamps(session.getOperationTime(), cloneTimestamp) < 0;
            const secondRes = session.getDatabase(dbName).runCommand(commandObj);
            const errorCtx = { commandObj, commandIdx, firstRes, secondRes, cloneTimestamp };
            shell$3.assert.eq(this.fetchGeneratedOplogEntries(shardConns), [], Object.assign({ message: 'retryable write generated an oplog entry upon retry' }, errorCtx));
            if (writeCommittedBeforeCloningStarted) {
                // Retryable writes committed before the resharding operation started are not able
                // to be retried after the resharding operation has finished.
                this.assertRespondedWithIncompleteHistory(commandObj, commandIdx, firstRes, secondRes);
            }
            else if (!haveSeenFirstWriteCommittedAfterCloningStarted) {
                // Mongos gossips the highest cluster time seen across the different shards. This
                // causes the shards to advance their logical clock when targeted by a call to
                // runCommand(). Shards will therefore always generate oplog entries with a
                // timestamp strictly higher than the timestamp of any oplog entry previously
                // generated by a call to runCommand(). This yields the so-called monotonic writes
                // property. We skip the first write with an operationTime >= cloneTimestamp because
                // it is possible for some of the statements to have had a
                // timestamp < cloneTimestamp assigned to their generated oplog entries. Due to the
                // monotonic writes property, it is sufficient to skip only the first write with an
                // operationTime >= cloneTimestamp. Note that without the monotonic writes property,
                // it would otherwise be possible for subsequent writes to have also straddled the
                // cloneTimestamp boundary.
                haveSeenFirstWriteCommittedAfterCloningStarted = true;
            }
            else {
                const commandName = this.extractCommandName(commandObj);
                if (commandName === 'findAndModify' && skipCheckSameResultForFindAndModify) {
                    continue;
                }
                // Retryable writes committed during the resharding operation are able to be retried
                // after the resharding operation has finished.
                this.assertRespondedWithSameWriteResult(commandObj, commandIdx, firstRes, secondRes);
            }
        }
        for (const { shardSession } of shardConns) {
            shardSession.endSession();
        }
    }
    establishDirectShardConnections() {
        const topology = shell$3.DiscoverTopology.findConnectedNodes(this.mongos);
        let highestClusterTime;
        const advanceOperationTimeAndClusterTime = (shardSession) => {
            if (highestClusterTime !== undefined) {
                shardSession.advanceClusterTime(highestClusterTime);
            }
            highestClusterTime = shardSession.getClusterTime();
            shardSession.advanceOperationTime(highestClusterTime.clusterTime);
        };
        return Object.entries(topology.shards)
            .map(([shardName, { primary }]) => {
            const shardSession = new shell$3.Mongo(primary).startSession({
                // Enable causal consistency for the shard's session because waiting for read
                // concern will also wait for all earlier oplog writes to be visible to a
                // forward-scanning oplog cursor.
                causalConsistency: true,
                retryWrites: false,
            });
            shell$3.assert.commandWorked(shardSession.getDatabase('admin').runCommand({ ping: 1 }));
            advanceOperationTimeAndClusterTime(shardSession);
            return { shardName, shardSession };
        })
            .map(({ shardName, shardSession }) => {
            // Do a second pass to set the highest cluster time seen across all sessions on all
            // sessions.
            advanceOperationTimeAndClusterTime(shardSession);
            return { shardName, shardSession };
        });
    }
    fetchGeneratedOplogEntries(shardConns) {
        return shardConns
            .map(({ shardName, shardSession }) => {
            const oplogCollection = shardSession.getDatabase('local').getCollection('oplog.rs');
            return {
                shardName,
                oplogEntries: oplogCollection
                    .aggregate([
                    {
                        $match: {
                            ts: { $gte: shardSession.getOperationTime() },
                            // Ignore no-op oplog entries.
                            op: { $ne: 'n' },
                            // Ignore oplog entries from the admin and config databases.
                            ns: /^(?!(admin|config)\.)[^.]+\./,
                        },
                    },
                ])
                    .toArray(),
            };
        })
            .filter(({ oplogEntries }) => oplogEntries.length > 0);
    }
    assertRespondedWithIncompleteHistory(commandObj, commandIdx, firstRes, secondRes) {
        const check = errInfo => errInfo.code === shell$3.ErrorCodes.IncompleteTransactionHistory;
        // Writes may end up not failing with an IncompleteTransactionHistory error response if the
        // statements are targeted to the same shards they were originally performed on after the
        // resharding operation finished.
        const commandName = this.extractCommandName(commandObj);
        const errorCtx = { commandObj, commandIdx, firstRes, secondRes };
        shell$3.assert(this.didRespondWithSameWriteResult(commandObj, commandIdx, firstRes, secondRes) ||
            (commandName === 'findAndModify' && check(secondRes)) ||
            (commandName !== 'findAndModify' &&
                secondRes.writeErrors !== undefined &&
                secondRes.writeErrors.every(writeError => check(writeError))), Object.assign({
            message: 'retryable write committed before the cloneTimestamp did not fail with an' +
                ' incomplete history error',
        }, errorCtx));
    }
    endSessions() {
        for (const { session } of this.responses) {
            session.endSession();
        }
    }
}

const shell$4 = globalThis;

{
    shell$4.DiscoverTopology = await (async () => {
        const {DiscoverTopology} = await import('jstests/libs/discover_topology.js');
        return DiscoverTopology;
    })();
    shell$4.ReshardingTest = await (async () => {
        const {ReshardingTest} = await import('jstests/sharding/libs/resharding_test_fixture.js');
        return ReshardingTest;
    })();
}

var FeatureFlagUtil = {isEnabled: () => false};
{
    // The feature_flag_util.js is introduced in 6.0 so we cannot use it in older versions. Since
    // sameKeyResharding is only true starting 7.1, we don't need to introduce another flag here
    // for now.
    FeatureFlagUtil = await (async () => {
        const {FeatureFlagUtil} = await import('jstests/libs/feature_flag_util.js');
        return FeatureFlagUtil;
    })();
}

const kPhases = {
    seed: 'seed',
    resharding: 'resharding',
};
/* eslint-enable no-empty-function */

function makeChunkRanges(donorOrRecipient, shardNames, numChunksPerShard, chunkRangeSize) {
    const chunks = [];

    chunks.push({
        min: {[donorOrRecipient]: shell$4.MinKey, slot: shell$4.MinKey},
        max: {[donorOrRecipient]: `${donorOrRecipient}0`, slot: shell$4.MinKey},
        shard: shardNames[0],
    });

    let min;
    for (let i = 0; i < shardNames.length; ++i) {
        min = {[donorOrRecipient]: `${donorOrRecipient}${i}`, slot: shell$4.MinKey};
        let max;

        for (let j = 0; j < numChunksPerShard - 1; ++j) {
            max = {
                [donorOrRecipient]: `${donorOrRecipient}${i}`,
                slot: min.slot !== shell$4.MinKey ? min.slot + chunkRangeSize : chunkRangeSize,
            };

            chunks.push({min: min, max: max, shard: shardNames[i]});
            min = max;
        }

        max = {[donorOrRecipient]: `${donorOrRecipient}${i + 1}`, slot: shell$4.MinKey};
        chunks.push({
            min: min,
            max: max,
            shard: shardNames[i],
        });
        min = max;
    }

    chunks.push({
        min: min,
        max: {[donorOrRecipient]: shell$4.MaxKey, slot: shell$4.MaxKey},
        shard: shardNames[0],
    });

    return chunks;
}

function setupFixture() {
    let reshardingTest;
    {
        reshardingTest = new shell$4.ReshardingTest({
            numDonors: numDonors,
            numRecipients: numRecipients,
            reshardInPlace: reshardInPlace,
            // The reshardingMinimumOperationDurationMillis setting must be long enough for
            // RetryableWriteManager.runCommand() to run the command twice in that amount of time.
            // Otherwise, RetryableWriteManager.runCommand() could fail due to an
            // IncompleteTransactionHistory error response on its retry attempt.
            minimumOperationDurationMS: 30 * 1000,
            enableElections: enableElections,
        });
    }
    reshardingTest.setup();

    const ns = `${kDbName}.${kCollName}`;
    function createSourceCollection() {

        return reshardingTest.createShardedCollection({
            ns: ns,
            shardKeyPattern: {donor: 1, slot: 1},
            chunks: makeChunkRanges(
                'donor',
                reshardingTest.donorShardNames,
                knobs.kNumChunksPerShard,
                knobs.kChunkRangeSize
            ),
        });
    }
    const sourceCollection = createSourceCollection();

    const mongos = sourceCollection.getMongo();
    const session = mongos.startSession({causalConsistency: false});

    const sessionOptions = {causalConsistency: false};
    const fuzzerName = TransactionManager.kResharding;

    const txnManager = new TransactionManager({
        primary: mongos,
        sessionOptions: sessionOptions,
        // Journaling write operations is not necessary as we don't SIGKILL any of the nodes.
        // This could change as part of PM-1079.
        journalPreparedTxns: false,
        fuzzerName: fuzzerName,
    });

    // We create kAbortFailedTransactionNs as a sharded collection with at least one chunk on each
    // shard. This ensures TransactionManager.abortFailedTransaction() will target all shards in the
    // cluster when it must unconditionally abort a failed transaction.
    shell$4.assert.commandWorked(
        mongos.adminCommand({enableSharding: kAbortFailedTransactionNs.split('.', 1)[0]})
    );
    shell$4.assert.commandWorked(
        mongos.adminCommand({
            shardCollection: kAbortFailedTransactionNs,
            key: {_id: 'hashed'},
            numInitialChunks: numDonors + numRecipients,
        })
    );

    const retryableWriteManager = new RetryableWriteManager(mongos);

    return {
        reshardingTest: reshardingTest,
        session: session,
        db: sourceCollection.getDB(),
        txnManager: txnManager,
        retryableWriteManager: retryableWriteManager,
    };
}

function teardownFixture(reshardingTest, txnManager, retryableWriteManager, session) {
    txnManager.endSessions();
    retryableWriteManager.endSessions();
    session.endSession();
    reshardingTest.teardown();
}

function printStats(stats, numSteps, name) {
    log('Stats for ' + name + ': ' + shell$4.tojson(Object.assign({numSteps: numSteps}, stats.raw)));
}

function recordSeedDataStats(res, op, stats) {
    if (res.ok !== 1) {
        // Operations will fail to acquire a lock if they are modifying a collection who's
        // lock is held by a prepared transaction. This is the only expected error, so
        // any other error should still fail the test.
        shell$4.assert.commandFailedWithCode(res, shell$4.ErrorCodes.LockTimeout);
    } else {
        shell$4.assert.commandWorked(res);
    }
    stats.recordDocumentInserted(op);
}

function recordReshardingStepsStats(res, op, stats) {
    stats.recordEvent(op, res);
}

function runOperation(db, op, stats, recordStatsCB, cmdIdx) {
    let {dbName, commandObj} = op;

    const res = db.getSiblingDB(dbName).runCommand(commandObj);
    log(
        `Completed cmd ${cmdIdx} on primary. RES: ${shell$4.tojson(
            res
        )}, DB: ${dbName}, COMMAND: ${shell$4.tojsononeline(commandObj)}`
    );

    recordStatsCB(res, op, stats);
}

function runRetryableOperation(retryableWriteManager, op, stats, recordStatsCB, cmdIdx) {
    const {dbName, commandObj} = op;

    const res = retryableWriteManager.runCommand(dbName, commandObj, cmdIdx);
    log(
        `Completed retryable write ${cmdIdx}. RESULT: ${shell$4.tojson(
            res
        )}, DB: ${dbName}, COMMAND: ${shell$4.tojsononeline(commandObj)}`
    );

    recordStatsCB(res, op, stats);
}

function runStep({
    reshardingTest,
    db,
    txnManager,
    retryableWriteManager,
    step,
    cmdIdx,
    stats,
    recordStatsCB,
}) {
    const runRetryableCommand = commandObj =>
        runRetryableOperation(
            retryableWriteManager,
            {dbName: kDbName, commandObj: commandObj},
            stats,
            recordStatsCB,
            cmdIdx
        );

    const {type} = step;
    switch (type) {
        case 'runTransaction': {
            const {ops} = step;
            txnManager.runTransaction(ops, cmdIdx);
            break;
        }
        case 'plain': {
            const {ops} = step;
            for (let op of ops) {
                runOperation(db, op, stats, recordStatsCB, cmdIdx);
            }
            break;
        }
        case 'retryableInsert': {
            const {documents} = step;
            runRetryableCommand({insert: kCollName, documents: documents});
            break;
        }
        case 'retryableUpdate': {
            const {updates} = step;
            runRetryableCommand({update: kCollName, updates: updates});
            break;
        }
        case 'retryableDelete': {
            const {deletes} = step;
            runRetryableCommand({delete: kCollName, deletes: deletes});
            break;
        }
        case 'retryableFindAndModify': {
            const {ops} = step;
            for (let op of ops) {
                runRetryableOperation(retryableWriteManager, op, stats, recordStatsCB, cmdIdx);
            }
            break;
        }
        case 'stepup':
        case 'terminate':
        case 'kill': {
            const {role, shard = 0} = step;
            const shardName = {
                donor: reshardingTest.donorShardNames,
                recipient: reshardingTest.recipientShardNames,
                coordinator: [reshardingTest.configShardName],
            }[role][shard];

            const methodName = {
                stepup: 'stepUpNewPrimaryOnShard',
                terminate: 'shutdownAndRestartPrimaryOnShard',
                kill: 'killAndRestartPrimaryOnShard',
            }[type];

            reshardingTest[methodName](shardName);
            break;
        }
        default:
            throw new Error('Unknown type: ' + type);
    }
}

function seedInitialData({db, txnManager, retryableWriteManager, stats}) {
    let stepNo = 0;

    while (stepNo < phaseChange) {
        const step = steps[stepNo];
        shell$4.assert.eq(step.phase, kPhases.seed);

        log('Executing seed step: ' + stepNo);
        runStep({
            db: db,
            txnManager: txnManager,
            retryableWriteManager: retryableWriteManager,
            step: step,
            cmdIdx: stepNo,
            stats: stats,
            recordStatsCB: recordSeedDataStats,
        });
        stepNo++;
    }

    return stepNo;
}

function runSteps({reshardingTest, db, txnManager, retryableWriteManager, stats}) {
    {
        shell$4.assert(
            FeatureFlagUtil.isEnabled(db.getMongo(), 'ReshardingImprovements') &&
                FeatureFlagUtil.isEnabled(
                    db.getMongo(),
                    'TrackUnshardedCollectionsOnShardingCatalog'
                ) &&
                FeatureFlagUtil.isEnabled(db.getMongo(), 'UnshardCollection'),
            'missing required feature flag(s).'
        );
    }

    let stepNo = phaseChange;
    let cloneTimestamp;

    function reshardingFn() {
        while (stepNo < steps.length) {
            const step = steps[stepNo];
            shell$4.assert.eq(step.phase, kPhases.resharding);

            log('Executing resharding fuzzer step: ' + stepNo);
            runStep({
                reshardingTest: reshardingTest,
                db: db,
                txnManager: txnManager,
                retryableWriteManager: retryableWriteManager,
                step: step,
                cmdIdx: stepNo,
                stats: stats,
                recordStatsCB: recordReshardingStepsStats,
            });
            stepNo++;
        }

        cloneTimestamp = reshardingTest.awaitCloneTimestampChosen();
    }

    {
        reshardingTest.withUnshardCollectionInBackground(
            {toShard: reshardingTest.recipientShardNames[0]},
            () => {
                reshardingFn();
            }
        );
    }

    return {numSteps: stepNo, cloneTimestamp: cloneTimestamp};
}

function checkServerStatusForTxns(db, txnStats) {
    const txnStatus = db.serverStatus().transactions;
    log('db.serverStatus().transactions stats for resharding: ' + shell$4.tojson(txnStatus));

    // Check that the expected number of transaction step types were executed. Retryable writes can
    // be implicitly converted into multi-document transactions so the statistics tracked by the
    // TransactionManager may end up undercounting.
    shell$4.assert.gte(txnStatus.totalStarted, txnStats.startTransaction, txnStats);
    shell$4.assert.gte(txnStatus.totalCommitted, txnStats.commitTransaction, txnStats);
    shell$4.assert.gte(txnStatus.totalAborted, txnStats.abortTransaction, txnStats);
    // TODO: mongos doesn't report "totalPrepared" in its server status metrics. Maybe
    // "commitTypes.twoPhaseCommit.initiated" would be equivalent?
    // shell.assert.gte(txnStatus.totalPrepared, txnStats.prepareTransaction, txnStats);
}

function reshardingMain({
    seedInitialDataF = seedInitialData,
    runStepsF = runSteps,
    printStatsF = printStats,
    checkServerStatusForTxnsF = checkServerStatusForTxns,
} = {}) {
    log.setup({prefix: '[ReshardingFuzzer] '});

    const statsTracker = new ReshardingStatsImpl();
    const seedDataStatsTracker = new SeedDataStatsImpl();

    const {reshardingTest, session, db, txnManager, retryableWriteManager} = setupFixture();

    const seedDataNumSteps = seedInitialDataF({
        db: db,
        txnManager: txnManager,
        retryableWriteManager: retryableWriteManager,
        stats: seedDataStatsTracker,
    });
    printStatsF(seedDataStatsTracker, seedDataNumSteps, 'seed data');

    const {numSteps: reshardingNumSteps, cloneTimestamp} = runStepsF({
        reshardingTest: reshardingTest,
        db: db,
        txnManager: txnManager,
        retryableWriteManager: retryableWriteManager,
        stats: statsTracker,
    });
    printStatsF(statsTracker, reshardingNumSteps - seedDataNumSteps, 'resharding');

    // We force the mongos to refresh the collection after the resharding operation has completed so
    // it is guaranteed to target the recipient shards for its retries of the retryable writes.
    const ns = `${kDbName}.${kCollName}`;
    shell$4.assert.commandWorked(db.adminCommand({flushRouterConfig: ns}));
    {
        retryableWriteManager.checkExactlyOnceSemantics(
            cloneTimestamp,
            skipCheckSameResultForFindAndModify
        );
    }

    const txnStats = txnManager.stats();
    checkServerStatusForTxnsF(db, txnStats);

    teardownFixture(reshardingTest, txnManager, retryableWriteManager, session);
}

reshardingMain();

