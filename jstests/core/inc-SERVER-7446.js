var c = db.incSERVER7446;

// A 32 bit overflow spills to 64 bits
c.drop();
c.save({a: NumberInt("2147483647")});
var updateResult = c.update({}, {$inc: {a: NumberInt(1)}});
assert.eq(1, updateResult.nMatched, "Object not modified");
var res = c.findOne();
assert.eq(NumberLong,
          res.a.constructor,
          "NumberInt incremented beyond std::numeric_limits<in32_t>::max() not NumberLong");
assert.eq(NumberLong("2147483648"),
          res.a,
          "NumberInt incremented beyond std::numeric_limits<in32_t>::max() has wrong value");

// A 32 bit underflow spills to 64 bits
c.drop();
c.save({a: NumberInt("-2147483648")});
updateResult = c.update({}, {$inc: {a: NumberInt(-1)}});
assert.eq(1, updateResult.nMatched, "Object not modified");
res = c.findOne();
assert.eq(NumberLong,
          res.a.constructor,
          "NumberInt decremented beyond std::numeric_limits<in32_t>::min() not NumberLong");
assert.eq(NumberLong("-2147483649"),
          res.a,
          "NumberInt decremented beyond std::numeric_limits<in32_t>::min() has wrong value");

// A 64 bit overflow is an error
c.drop();
c.save({a: NumberLong("9223372036854775807")});
updateResult = c.update({}, {$inc: {a: NumberInt(1)}});
assert.eq(0,
          updateResult.nMatched,
          "Did not fail to increment a NumberLong past std::numeric_limits<int64_t>::max()");

// A 64 bit underflow is an error
c.drop();
c.save({a: NumberLong("-9223372036854775808")});
updateResult = c.update({}, {$inc: {a: NumberInt(-1)}});
assert.eq(0,
          updateResult.nMatched,
          "Did not fail to decrement a NumberLong past std::numeric_limits<int64_t>::min()");

c.drop();
