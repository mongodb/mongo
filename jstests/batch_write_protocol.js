//
// Ensures that mongod respects the basic batch write protocols
//

var coll = db.getCollection("batch_write_protocol");

var PRESENT = "PRESENT";
var ABSENT = "ABSENT";

var assertHasFields = function(docA, docB) {
    print("checking " + tojson(docA) + " vs. " + tojson(docB));

    for ( var field in docB) {
        if (docB[field] === PRESENT && docA[field] === undefined)
            assert(false, "field is absent that shouldn't be: " + field);
    }

    for ( var field in docA) {
        if (docB[field] === undefined) continue;
        if (docB[field] === ABSENT) assert(false, "field is present that shouldn't be: " + field);
        if (docB[field] === PRESENT) continue;
        assert.eq(docA[field], docB[field]);
    }
};

var request;
var response;

//
// INSERT
//

jsTest.log("Starting insert tests...");

coll.remove({});

// Only check the parsing lightly, we do this elsewhere
// NO DOCS
request = {insert : coll + "", writeConcern : {}, continueOnError : true};
response = {ok : false, errCode : PRESENT, errMessage : PRESENT};
assertHasFields(coll.runCommand(request), response);

// NO WC
request = {insert : coll + "", documents : [{a : 1}], continueOnError : true};
response = {ok : false, errCode : PRESENT, errMessage : PRESENT};
assertHasFields(coll.runCommand(request), response);

// NO COE
request = {insert : coll + "", documents : [{a : 1}], writeConcern : {}};
response = {ok : false, errCode : PRESENT, errMessage : PRESENT};
assertHasFields(coll.runCommand(request), response);

// Correct single insert
request = {insert : coll + "", documents : [{a : 1}], writeConcern : {}, continueOnError : true};
response = {ok : true, errCode : ABSENT, errMessage : ABSENT, n : 1};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 1);

// Correct multi insert
request = {insert : coll + "",
           documents : [{a : 2}, {a : 3}],
           writeConcern : {},
           continueOnError : true};
response = {ok : true, errCode : ABSENT, errMessage : ABSENT, n : 2};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Error on single insert
coll.ensureIndex({a : 1}, {unique : true});
request = {insert : coll + "", documents : [{a : 1}], writeConcern : {}, continueOnError : true};
response = {ok : false, errCode : PRESENT, errMessage : PRESENT};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Error on multi insert
request = {insert : coll + "",
           documents : [{a : 1}, {a : 4}],
           writeConcern : {},
           continueOnError : true};
response = {ok : false, errCode : PRESENT, errMessage : PRESENT, errDetails : PRESENT};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 4);

// Error on multi insert, no COE
request = {insert : coll + "",
           documents : [{a : 1}, {a : 5}],
           writeConcern : {},
           continueOnError : false};
response = {ok : false, errCode : PRESENT, errMessage : PRESENT, errDetails : PRESENT};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 4);

//
// Update
//

jsTest.log("Starting update tests...");

coll.remove({});

// Correct single update
request = {update : coll + "",
           updates : [{q : {a : 1}, u : {a : 1}}],
           writeConcern : {},
           continueOnError : true};
response = {ok : true, errCode : ABSENT, errMessage : ABSENT, n : 0, upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 0);

// Correct single upsert
request = {update : coll + "",
           updates : [{q : {a : 1}, u : {a : 1}, upsert : true}],
           writeConcern : {},
           continueOnError : true};
response = {ok : true, errCode : ABSENT, errMessage : ABSENT, n : 0, upserted : 1};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 1);

// Correct multiple upsert
request = {update : coll + "",
           updates : [{q : {a : 2}, u : {a : 2}, upsert : true},
                      {q : {a : 3}, u : {a : 3}, upsert : true}],
           writeConcern : {},
           continueOnError : true};
response = {ok : true, errCode : ABSENT, errMessage : ABSENT, n : 0, upserted : 2};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Correct multiple update
request = {update : coll + "",
           updates : [{q : {}, u : {$set : {b : 1}}, multi : true}],
           writeConcern : {},
           continueOnError : true};
response = {ok : true, errCode : ABSENT, errMessage : ABSENT, n : 3, upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Error on single update
request = {update : coll + "",
           updates : [{q : {}, u : {b : 1}, multi : true}],
           writeConcern : {},
           continueOnError : true};
response = {ok : false, errCode : PRESENT, errMessage : PRESENT};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Error on multiple update
request = {update : coll + "",
           updates : [{q : {}, u : {b : 1}, multi : true},
                      {q : {}, u : {$set : {b : 2}}, multi : true}],
           writeConcern : {},
           continueOnError : true};
response = {ok : false,
            errCode : PRESENT,
            errMessage : PRESENT,
            errDetails : PRESENT,
            n : 3,
            upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Error on multiple update, no COE
request = {update : coll + "",
           updates : [{q : {}, u : {b : 1}, multi : true},
                      {q : {}, u : {$set : {b : 2}}, multi : true}],
           writeConcern : {},
           continueOnError : false};
response = {ok : false,
            errCode : PRESENT,
            errMessage : PRESENT,
            errDetails : PRESENT,
            n : 0,
            upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

//
// Delete
//

jsTest.log( "Starting delete tests..." );

// Correct single delete
request = {delete : coll + "",
           deletes : [{q : {a : 1}}],
           writeConcern : {},
           continueOnError : true};
response = {ok : true, errCode : ABSENT, errMessage : ABSENT, n : 1, upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 2);

// Correct multi delete
request = {delete : coll + "",
           deletes : [{q : {a : 2}}, { q : { a : 10}}],
           writeConcern : {},
           continueOnError : true};
response = {ok : true, errCode : ABSENT, errMessage : ABSENT, n : 1, upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 1);

// Error on single delete
request = {delete : coll + "",
           deletes : [{q : {$set : {a : 2}}}],
           writeConcern : {},
           continueOnError : true};
response = {ok : false, errCode : PRESENT, errMessage : PRESENT};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 1);

 // Error on multi delete
request = {delete : coll + "",
           deletes : [{q : {$set : {a : 2}}},
                      { q : { a : 3 } }],
           writeConcern : {}, 
           continueOnError : true};
 response = {ok : false, 
             errCode : PRESENT, 
             errMessage : PRESENT,
             errDetails : PRESENT,
             n : 1, 
             upserted : 0};
 assertHasFields(coll.runCommand(request), response); 
 assert.eq(coll.count(), 0);
 
// Error on multi delete, no COE
coll.insert({ a : 3 });
request = {delete : coll + "",
           deletes : [{q : {$set : {a : 2}}},
                      {q : { a : 3 }}],
           writeConcern : {},
           continueOnError : false};
response = {ok : false,
            errCode : PRESENT,
            errMessage : PRESENT,
            errDetails : PRESENT,
            n : 0, 
            upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 1);

