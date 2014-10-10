// big numeric updates (used to overflow)

t = db.updatee;
t.drop();

var o = { "_id" : 1, 
          "actual" : { 
        "key1" : "val1", 
        "key2" : "val2", 
        "001" : "val3", 
        "002" : "val4", 
        "0020000000000000000000" : "val5" 
    }, 
          "profile-id" : "test" };


t.insert( o );
assert.eq( o , t.findOne() , "A1" );

t.update({"profile-id" : "test"}, {$set: {"actual.0030000000000000000000": "val6"}});

var q = t.findOne();

// server-1347
assert.eq(q.actual["0020000000000000000000"], "val5", "A2");
assert.eq(q.actual["0030000000000000000000"], "val6", "A3");

t.update({"profile-id" : "test"}, {$set: {"actual.02": "v4"}});

q = t.findOne();
assert.eq(q.actual["02"], "v4", "A4");
assert.eq(q.actual["002"], "val4", "A5");

t.update({"_id" : 1}, {$set : {"actual.2139043290148390248219423941.b" : 4}});
q = t.findOne();
assert.eq(q.actual["2139043290148390248219423941"].b, 4, "A6");

// non-nested
t.update({"_id" : 1}, {$set : {"7213647182934612837492342341" : 1}});
t.update({"_id" : 1}, {$set : {"7213647182934612837492342342" : 2}});

q = t.findOne();
assert.eq(q["7213647182934612837492342341"], 1, "A7 1");
assert.eq(q["7213647182934612837492342342"], 2, "A7 2");

// 0s
t.update({"_id" : 1}, {$set : {"actual.000" : "val000"}});
q = t.findOne();
assert.eq(q.actual["000"], "val000", "A8 zeros");

t.update({"_id" : 1}, {$set : {"actual.00" : "val00"}});
q = t.findOne();
assert.eq(q.actual["00"], "val00", "A8 00");
assert.eq(q.actual["000"], "val000", "A9");

t.update({"_id" : 1}, {$set : {"actual.000" : "val000"}});
q = t.findOne();
assert.eq(q.actual["000"], "val000", "A9");
assert.eq(q.actual["00"], "val00", "A10");

t.update({"_id" : 1}, {$set : {"actual.01" : "val01"}});
q = t.findOne();
assert.eq(q.actual["000"], "val000", "A11");
assert.eq(q.actual["01"], "val01", "A12");

// shouldn't work, but shouldn't do anything too heinous, either
t.update({"_id" : 1}, {$set : {"0.." : "val01"}});
t.update({"_id" : 1}, {$set : {"0..0" : "val01"}});
t.update({"_id" : 1}, {$set : {".0" : "val01"}});
t.update({"_id" : 1}, {$set : {"..0" : "val01"}});
t.update({"_id" : 1}, {$set : {"0.0..0" : "val01"}});
