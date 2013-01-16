// Checking for positional array updates with either .$ or .0 at the end
// SERVER-7511

// array.$.name
t = db.jstests_update_arraymatch8;
t.drop();
t.ensureIndex( {'array.name': 1} );
t.insert( {'array': [{'name': 'old'}]} );
assert( t.findOne({'array.name': 'old'}) );
t.update( {'array.name': 'old'}, {$set: {'array.$.name': 'new'}} );
assert( t.findOne({'array.name': 'new'}) );
assert( !t.findOne({'array.name': 'old'}) );

// array.$   (failed in 2.2.2)
t = db.jstests_update_arraymatch8;
t.drop();
t.ensureIndex( {'array.name': 1} );
t.insert( {'array': [{'name': 'old'}]} );
assert( t.findOne({'array.name': 'old'}) );
t.update( {'array.name': 'old'}, {$set: {'array.$': {'name':'new'}}} );
assert( t.findOne({'array.name': 'new'}) );
assert( !t.findOne({'array.name': 'old'}) );

// array.0.name
t = db.jstests_update_arraymatch8;
t.drop();
t.ensureIndex( {'array.name': 1} );
t.insert( {'array': [{'name': 'old'}]} );
assert( t.findOne({'array.name': 'old'}) );
t.update( {'array.name': 'old'}, {$set: {'array.0.name': 'new'}} );
assert( t.findOne({'array.name': 'new'}) );
assert( !t.findOne({'array.name': 'old'}) );

// array.0   (failed in 2.2.2)
t = db.jstests_update_arraymatch8;
t.drop();
t.ensureIndex( {'array.name': 1} );
t.insert( {'array': [{'name': 'old'}]} );
assert( t.findOne({'array.name': 'old'}) );
t.update( {'array.name': 'old'}, {$set: {'array.0': {'name':'new'}}} );
assert( t.findOne({'array.name': 'new'}) );
assert( !t.findOne({'array.name': 'old'}) );

// // array.12.name
t = db.jstests_update_arraymatch8;
t.drop();
arr = new Array();
for (var i=0; i<20; i++) {
    arr.push({'name': 'old'});
}
t.ensureIndex( {'array.name': 1} );
t.insert( {_id:0, 'array': arr} );
assert( t.findOne({'array.name': 'old'}) );
t.update( {_id:0}, {$set: {'array.12.name': 'new'}} );
// note: both documents now have to be in the array
assert( t.findOne({'array.name': 'new'}) );
assert( t.findOne({'array.name': 'old'}) );

// array.12   (failed in 2.2.2)
t = db.jstests_update_arraymatch8;
t.drop();
arr = new Array();
for (var i=0; i<20; i++) {
    arr.push({'name': 'old'});
}
t.ensureIndex( {'array.name': 1} );
t.insert( {_id:0, 'array': arr} );
assert( t.findOne({'array.name': 'old'}) );
t.update( {_id:0}, {$set: {'array.12': {'name':'new'}}} );
// note: both documents now have to be in the array
assert( t.findOne({'array.name': 'new'}) );
assert( t.findOne({'array.name': 'old'}) );

// array.$.123a.name
t = db.jstests_update_arraymatch8;
t.drop();
t.ensureIndex( {'array.123a.name': 1} );
t.insert( {'array': [{'123a':{'name': 'old'}}]} );
assert( t.findOne({'array.123a.name': 'old'}) );
t.update( {'array.123a.name': 'old'}, {$set: {'array.$.123a.name': 'new'}} );
assert( t.findOne({'array.123a.name': 'new'}) );
assert( !t.findOne({'array.123a.name': 'old'}) );

// array.$.123a
t = db.jstests_update_arraymatch8;
t.drop();
t.ensureIndex( {'array.name': 1} );
t.insert( {'array': [{'123a':{'name': 'old'}}]} );
assert( t.findOne({'array.123a.name': 'old'}) );
t.update( {'array.123a.name': 'old'}, {$set: {'array.$.123a': {'name': 'new'}}} );
assert( t.findOne({'array.123a.name': 'new'}) );
assert( !t.findOne({'array.123a.name': 'old'}) );

// array.0.123a.name
t = db.jstests_update_arraymatch8;
t.drop();
t.ensureIndex( {'array.123a.name': 1} );
t.insert( {'array': [{'123a':{'name': 'old'}}]} );
assert( t.findOne({'array.123a.name': 'old'}) );
t.update( {'array.123a.name': 'old'}, {$set: {'array.0.123a.name': 'new'}} );
assert( t.findOne({'array.123a.name': 'new'}) );
assert( !t.findOne({'array.123a.name': 'old'}) );

// array.0.123a
t = db.jstests_update_arraymatch8;
t.drop();
t.ensureIndex( {'array.name': 1} );
t.insert( {'array': [{'123a':{'name': 'old'}}]} );
assert( t.findOne({'array.123a.name': 'old'}) );
t.update( {'array.123a.name': 'old'}, {$set: {'array.0.123a': {'name': 'new'}}} );
assert( t.findOne({'array.123a.name': 'new'}) );
assert( !t.findOne({'array.123a.name': 'old'}) );

