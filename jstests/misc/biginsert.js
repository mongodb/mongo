o = "xxxxxxxxxxxxxxxxxxx";
o = o + o;
o + o;
o = o + o;
o = o + o;
o = o + o;

var B = 40000;
var last = new Date();
for (i = 0; i < 30000000; i++) {
    db.foo.insert({ o: o });
    if (i % B == 0) {
        var n = new Date();
        print(i);
        print("per sec: " + B*1000 / (n - last));
        last = n;
    }
}
