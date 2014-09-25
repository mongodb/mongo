var t = db.regex_limit;
t.drop();

var repeatStr = function(str, n){
  return new Array(n + 1).join(str);
};

t.insert({ z: repeatStr('c', 100000) });

var maxOkStrLen = repeatStr('c', 32764);
var strTooLong = maxOkStrLen + 'c';

assert(t.findOne({ z: { $regex: maxOkStrLen }}) != null);
assert.throws(function() {
    t.findOne({ z: { $regex: strTooLong }});
});

assert(t.findOne({ z: { $in: [ new RegExp(maxOkStrLen) ]}}) != null);
assert.throws(function() {
    t.findOne({ z: { $in: [ new RegExp(strTooLong) ]}});
});

