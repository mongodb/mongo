t = db.find7;
t.drop();

x = {
    "_id": {"d": 3649, "w": "signed"},
    "u": {"3649": 5}
};
t.insert(x);
assert.eq(x, t.findOne(), "A1");
assert.eq(x, t.findOne({_id: x._id}), "A2");
