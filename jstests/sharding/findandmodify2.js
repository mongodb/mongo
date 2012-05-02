var s = new ShardingTest({ name: "find_and_modify_sharded_2", shards: 2, verbose: 2, mongos: 1, other: { chunksize: 1 }});
s.adminCommand( { enablesharding : "test" } );

var db = s.getDB( "test" );
var primary = s.getServer( "test" ).getDB( "test" );
var secondary = s.getOther( primary ).getDB( "test" );

var n = 100;
var collection = "stuff";
var minChunks = 2;

var col_update = collection + '_col_update';
var col_update_upsert = col_update + '_upsert';
var col_fam = collection + '_col_fam';
var col_fam_upsert = col_fam + '_upsert';

var big = "x";

print("---------- Creating large payload...");
for(var i=0;i<15;i++) {
    big += big;
}
print("---------- Done.");

// drop the collection
db[col_update].drop();
db[col_update_upsert].drop();
db[col_fam].drop();
db[col_fam_upsert].drop();

// shard the collection on _id
s.adminCommand({shardcollection: 'test.' + col_update, key: {_id: 1}});
s.adminCommand({shardcollection: 'test.' + col_update_upsert, key: {_id: 1}});
s.adminCommand({shardcollection: 'test.' + col_fam, key: {_id: 1}});
s.adminCommand({shardcollection: 'test.' + col_fam_upsert, key: {_id: 1}});

// update via findAndModify
function via_fam() {
  for (var i=0; i<n; i++){
    db[col_fam].save({ _id: i });
  }

  for (var i=0; i<n; i++){
    db[col_fam].findAndModify({query: {_id: i}, update: { $set:
        { big: big }
    }});
  }
}

// upsert via findAndModify
function via_fam_upsert() {
  for (var i=0; i<n; i++){
    db[col_fam_upsert].findAndModify({query: {_id: i}, update: { $set:
        { big: big }
    }, upsert: true});
  }
}

// update data using basic update
function via_update() {
  for (var i=0; i<n; i++){
    db[col_update].save({ _id: i });
  }

  for (var i=0; i<n; i++){
    db[col_update].update({_id: i}, { $set:
        { big: big }
    });
  }
}

// upsert data using basic update
function via_update_upsert() {
  for (var i=0; i<n; i++){
    db[col_update_upsert].update({_id: i}, { $set:
        { big: big }
    }, true);
  }
}

print("---------- Update via findAndModify...");
via_fam();
print("---------- Done.");

print("---------- Upsert via findAndModify...");
via_fam_upsert();
print("---------- Done.");

print("---------- Basic update...");
via_update();
print("---------- Done.");

print("---------- Basic update with upsert...");
via_update_upsert();
print("---------- Done.");

print("---------- Printing chunks:");
s.printChunks();


print("---------- Verifying that both codepaths resulted in splits...");
assert.gt( s.config.chunks.count({ "ns": "test." + col_fam }), minChunks, "findAndModify update code path didn't result in splits" );
assert.gt( s.config.chunks.count({ "ns": "test." + col_fam_upsert }), minChunks, "findAndModify upsert code path didn't result in splits" );
assert.gt( s.config.chunks.count({ "ns": "test." + col_update }), minChunks, "update code path didn't result in splits" );
assert.gt( s.config.chunks.count({ "ns": "test." + col_update_upsert }), minChunks, "upsert code path didn't result in splits" );

// ensure that all chunks are smaller than chunksize
// make sure not teensy
// test update without upsert and with upsert

s.stop();
