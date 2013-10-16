//
// Tests batch writes in a sharded cluster
//

var options = {separateConfig : true};

var st = new ShardingTest({shards : 1, mongos : 1, other : options});
st.stopBalancer();

var mongos = st.s0;
var shards = mongos.getDB("config").shards.find().toArray();
var admin = mongos.getDB("admin");
var coll = mongos.getCollection("foo.bar");

assert(admin.runCommand({enableSharding : coll.getDB() + ""}).ok);
printjson(admin.runCommand({movePrimary : coll.getDB() + "", to : shards[0]._id}));

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
assert.eq( null, coll.getDB().getLastError() );
assert(admin.runCommand({ shardCollection : coll + "", key : {a : 1} }).ok);

// Only check the parsing lightly, we do this elsewhere
// NO DOCS
request = {insert : coll.getName(), writeConcern : {}, ordered : false};
response = {ok : false, errCode : PRESENT, errMsg : PRESENT};
assertHasFields(coll.runCommand(request), response);

// NO WC
request = {insert : coll.getName(), documents : [{a : 1}], ordered : false};
response = {ok : false, errCode : PRESENT, errMsg : PRESENT};
assertHasFields(coll.runCommand(request), response);

// NO COE
request = {insert : coll.getName(), documents : [{a : 1}], writeConcern : {}};
response = {ok : false, errCode : PRESENT, errMsg : PRESENT};
assertHasFields(coll.runCommand(request), response);

// Correct single insert
request = {insert : coll.getName(), documents : [{a : 1}], writeConcern : {}, ordered : false};
response = {ok : true, errCode : ABSENT, errMsg : ABSENT, n : 1};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 1);

// Correct multi insert
request = {insert : coll.getName(),
           documents : [{a : 2}, {a : 3}],
           writeConcern : {},
           ordered : false};
response = {ok : true, errCode : ABSENT, errMsg : ABSENT, n : 2};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Error on single insert
st.shard0.getCollection( coll + "" ).dropIndex({a : 1});
st.shard0.getCollection( coll + "" ).ensureIndex({a : 1}, {unique : true});
request = {insert : coll.getName(), documents : [{a : 1}], writeConcern : {}, ordered : false};
response = {ok : false, errCode : PRESENT, errMsg : PRESENT};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Error on multi insert
request = {insert : coll.getName(),
           documents : [{a : 1}, {a : 4}],
           writeConcern : {},
           ordered : false};
response = {ok : false, errCode : PRESENT, errMsg : PRESENT, errDetails : PRESENT};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 4);

// Error on multi insert, no COE
request = {insert : coll.getName(),
           documents : [{a : 1}, {a : 5}],
           writeConcern : {},
           ordered : true};
response = {ok : false, errCode : PRESENT, errMsg : PRESENT, errDetails : PRESENT};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 4);

//
// Update
//

jsTest.log("Starting update tests...");

coll.remove({});
assert.eq( null, coll.getDB().getLastError() );
st.shard0.getCollection( coll + "" ).dropIndex({a : 1});
st.shard0.getCollection( coll + "" ).ensureIndex({a : 1});

// Correct single update
request = {update : coll.getName(),
           updates : [{q : {a : 1}, u : {a : 1}}],
           writeConcern : {},
           ordered : false};
response = {ok : true, errCode : ABSENT, errMsg : ABSENT, n : 0, upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 0);

// Correct single upsert
request = {update : coll.getName(),
           updates : [{q : {a : 1}, u : {a : 1}, upsert : true}],
           writeConcern : {},
           ordered : false};
response = {ok : true, errCode : ABSENT, errMsg : ABSENT, n : 0, upserted : 1};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 1);

// Correct multiple upsert
request = {update : coll.getName(),
           updates : [{q : {a : 2}, u : {a : 2}, upsert : true},
                      {q : {a : 3}, u : {a : 3}, upsert : true}],
           writeConcern : {},
           ordered : false};
response = {ok : true, errCode : ABSENT, errMsg : ABSENT, n : 0, upserted : 2};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Correct multiple update
request = {update : coll.getName(),
           updates : [{q : {}, u : {$set : {b : 1}}, multi : true}],
           writeConcern : {},
           ordered : false};
response = {ok : true, errCode : ABSENT, errMsg : ABSENT, n : 3, upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Error on single update
request = {update : coll.getName(),
           updates : [{q : {}, u : {b : 1}, multi : true}],
           writeConcern : {},
           ordered : false};
response = {ok : false, errCode : PRESENT, errMsg : PRESENT};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Error on multiple update
request = {update : coll.getName(),
           updates : [{q : {}, u : {b : 1}, multi : true},
                      {q : {}, u : {$set : {b : 2}}, multi : true}],
           writeConcern : {},
           ordered : false};
response = {ok : false,
            errCode : PRESENT,
            errMsg : PRESENT,
            errDetails : PRESENT,
            n : 3,
            upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 3);

// Error on multiple update, no COE
request = {update : coll.getName(),
           updates : [{q : {}, u : {b : 1}, multi : true},
                      {q : {}, u : {$set : {b : 2}}, multi : true}],
           writeConcern : {},
           ordered : true};
response = {ok : false,
            errCode : PRESENT,
            errMsg : PRESENT,
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
request = {delete : coll.getName(),
        deletes : [{q : {a : 1}}],
        writeConcern : {},
        ordered : false};
response = {ok : true, errCode : ABSENT, errMsg : ABSENT, n : 1, upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 2);

// Correct multi delete
request = {delete : coll.getName(),
        deletes : [{q : {a : 2}}, { q : { a : 10}}],
        writeConcern : {},
        ordered : false};
response = {ok : true, errCode : ABSENT, errMsg : ABSENT, n : 1, upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 1);

// Error on single delete
request = {delete : coll.getName(),
        deletes : [{q : {$set : {a : 2}}}],
        writeConcern : {},
        ordered : false};
response = {ok : false, errCode : PRESENT, errMsg : PRESENT};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 1);

// Error on multi delete
request = {delete : coll.getName(),
        deletes : [{q : {$set : {a : 2}}},
                   { q : { a : 3 } }],
        writeConcern : {}, 
        ordered : false};
response = {ok : false, 
          errCode : PRESENT, 
          errMsg : PRESENT,
          errDetails : PRESENT,
          n : 1, 
          upserted : 0};
assertHasFields(coll.runCommand(request), response); 
assert.eq(coll.count(), 0);

// Error on multi delete, no COE
coll.insert({ a : 3 });
assert.eq( null, coll.getDB().getLastError() );
request = {delete : coll.getName(),
        deletes : [{q : {$set : {a : 2}}},
                   {q : { a : 3 }}],
        writeConcern : {},
        ordered : true};
response = {ok : false,
         errCode : PRESENT,
         errMsg : PRESENT,
         errDetails : PRESENT,
         n : 0, 
         upserted : 0};
assertHasFields(coll.runCommand(request), response);
assert.eq(coll.count(), 1);
