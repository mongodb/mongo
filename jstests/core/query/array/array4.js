/**
 * Basic test for querying on documents containing arrays.
 */
(function() {
'use strict';

const collNamePrefix = 'array4_';
let collCount = 0;

let t = db.getCollection(collNamePrefix + collCount++);
t.drop();

t.insert({_id: 0, 'a': ['1', '2', '3']});
t.insert({_id: 1, 'a': ['2', '1']});

let query = {'a.0': /1/};

let docs = t.find(query).toArray();
assert.eq(docs.length, 1, tojson(docs));

assert.eq(docs[0].a[0], 1, tojson(docs));
assert.eq(docs[0].a[1], 2, tojson(docs));

t = db.getCollection(collNamePrefix + collCount++);
t.drop();

t.insert({_id: 2, 'a': {'0': '1'}});
t.insert({_id: 3, 'a': ['2', '1']});

docs = t.find(query).toArray();
assert.eq(docs.length, 1, tojson(docs));
assert.eq(docs[0].a[0], 1, tojson(docs));

t = db.getCollection(collNamePrefix + collCount++);
t.drop();

t.insert({_id: 4, 'a': ['0', '1', '2', '3', '4', '5', '6', '1', '1', '1', '2', '3', '2', '1']});
t.insert({_id: 5, 'a': ['2', '1']});

query = {
    'a.12': /2/
};

docs = t.find(query).toArray();
assert.eq(docs.length, 1, tojson(docs));
assert.eq(docs[0].a[0], 0, tojson(docs));
}());
