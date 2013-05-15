//
// Tests 2dsphere with descending fields, ensures correct lookup
//

var coll = db.getCollection("twodspheredesc");

var descriptors = [["field1", -1], ["field2", -1], ["coordinates", "2dsphere"]]
var docA = {field1 : "a", field2 : 1, coordinates : [-118.2400013, 34.073893]}
var docB = {field1 : "b", field2 : 1, coordinates : [-118.2400012, 34.073894]}

// Try both regular and near index cursors
var query = {coordinates : {$geoWithin : {$centerSphere : [[-118.240013, 34.073893],
                                                           0.44915760491198753]}}};
var queryNear = {coordinates : {$geoNear : {"type" : "Point", "coordinates" : [0, 0]}}};

//
// The idea here is we try "2dsphere" indexes in combination with descending
// other fields in various
// positions and ensure that we return correct results.
//

for ( var t = 0; t < descriptors.length; t++) {

    var descriptor = {};
    for ( var i = 0; i < descriptors.length; i++) {
        descriptor[descriptors[i][0]] = descriptors[i][1];
    }

    jsTest.log("Trying 2dsphere index with descriptor " + tojson(descriptor));

    coll.drop();
    coll.ensureIndex(descriptor);

    coll.insert(docA);
    coll.insert(docB);

    assert.eq(1, coll.count(Object.merge(query, {field1 : "a"})));
    assert.eq(1, coll.count(Object.merge(query, {field1 : "b"})));
    assert.eq(2, coll.count(Object.merge(query, {field2 : 1})));
    assert.eq(0, coll.count(Object.merge(query, {field2 : 0})));
    assert.eq(2, coll.count(queryNear));

    var firstEls = descriptors.splice(1);
    descriptors = firstEls.concat(descriptors);
}

//
// Data taken from previously-hanging result
//

jsTest.log("Trying case found in wild...");

coll.drop();
coll.ensureIndex({coordinates : "2dsphere", field : -1});
coll.insert({coordinates : [-118.240013, 34.073893]});
var query = {coordinates : {$geoWithin : {$centerSphere : [[-118.240013, 34.073893],
                                                           0.44915760491198753]}},
             field : 1};

assert.eq(null, coll.findOne(query));
coll.remove({})
coll.insert({coordinates : [-118.240013, 34.073893], field : 1});
assert.neq(null, coll.findOne(query));

jsTest.log("Success!");
