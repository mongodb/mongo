//Shell version of Clock Benchmark: https://bug548388.bugzilla.mozilla.org/attachment.cgi?id=434576

var t0;
var tl;

function alloc(dt) {
    if (dt > 100)
        dt = 100;
    for (var i = 0; i < dt * 1000; ++i) {
        var o = new String("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    }
}

function cycle() {
    if (!running)
        return;

    var t1 = new Date;
    if (t0 == undefined) t0 = t1;

    if (tl != undefined) {
        var dt = t1 - tl;
        alloc(dt);
    }

    tl = t1;

    if(t1 - t0 > (5 * 1000))
        running = false;
}

var running = true;
while(running)
    cycle();

