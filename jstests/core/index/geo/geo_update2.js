// @tags: [
//   requires_multi_updates,
//   requires_non_retryable_writes,
// ]

let t = db.geo_update2;
t.drop();

for (let x = 0; x < 10; x++) {
    for (let y = 0; y < 10; y++) {
        t.insert({"loc": [x, y], x: x, y: y});
    }
}

t.createIndex({loc: "2d"});

function p() {
    print("--------------");
    for (let y = 0; y < 10; y++) {
        let c = t.find({y: y}).sort({x: 1});
        let s = "";
        while (c.hasNext()) s += c.next().z + " ";
        print(s);
    }
    print("--------------");
}

p();

assert.commandWorked(t.update({"loc": {"$within": {"$center": [[5, 5], 2]}}}, {"$inc": {"z": 1}}, false, true));
p();

assert.commandWorked(t.update({}, {"$inc": {"z": 1}}, false, true));
p();

assert.commandWorked(t.update({"loc": {"$within": {"$center": [[5, 5], 2]}}}, {"$inc": {"z": 1}}, false, true));
p();
