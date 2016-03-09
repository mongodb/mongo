
t = db.update_addToSet2;
t.drop();

o = {
    _id: 1
};
t.insert({_id: 1});

t.update({}, {$addToSet: {'kids': {'name': 'Bob', 'age': '4'}}});
t.update({}, {$addToSet: {'kids': {'name': 'Dan', 'age': '2'}}});

printjson(t.findOne());
