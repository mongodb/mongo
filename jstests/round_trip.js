// Tests to make sure conversions back and forth from BSON to V8
// objects round trip properly.

function ensureRoundTrip(value, bsonType, inArray) {
  var coll = db.round_trip;
  coll.drop();

  // insert the value into the collection
  // if inArray is set, we will nest the value in an array
  var before = inArray ? { val: [ value ] } : { val: value };
  coll.insert(before);

  // assert number was inserted as correct bson type
  assert.eq(coll.count({ val: { '$type': bsonType } }), 1);

  // assert number created with the correct function template
  var after = coll.findOne();
  var newval = inArray ? after.val[0] : after.val;
  assert.eq(value.constructor, newval.constructor);

  // assert number is re-inserted with the correct bson type
  var copy = {};
  copy.val = after.val;

  // NOTE: MongoDB's shell lazily decodes BSON objects so as to
  // not incur the cost of re-encoding them if not necessary.
  //
  // We make a copy here, by creating a new object and copying
  // the data value, in order to force the shell to re-encode
  // the document when it gets serialized.
  //
  // If this step is not taken, the original BSON is used and
  // the serialization/conversion process is not fully tested.
  //
  // This is NOT true for documents that contain arrays which
  // pre-emptively unpack the BSON instead of lazily doing it
  // because it produces performance benefits in v8.

  coll.drop();
  coll.insert(copy);
  assert.eq(coll.count({ val: { '$type' : bsonType } }), 1);
}

// Double -- BSON Type 1
ensureRoundTrip(Number(123), 1, false);
ensureRoundTrip(Number(123), 1, true);

// NumberInt (int32) -- BSON Type 16
ensureRoundTrip(NumberInt(123), 16, false);
ensureRoundTrip(NumberInt(123), 16, true);

// NumberLong (int64) -- BSON Type 18
ensureRoundTrip(NumberLong(12), 18, false);
ensureRoundTrip(NumberLong(12), 18, true);
