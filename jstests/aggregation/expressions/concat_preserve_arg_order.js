// make sure $concat doesn't optimize constants to the end
let c = db.c;
c.drop();

c.save({x: "3"});

let project = {$project: {a: {$concat: ["1", {$concat: ["foo", "$x", "bar"]}, "2"]}}};

assert.eq("1foo3bar2", c.aggregate(project).toArray()[0].a);
