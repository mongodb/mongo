//Benchmark to measure overhead of dslots allocation and deallocation

function Object0() {};
function Object1() { this.a=1; };
function Object2() { this.a=1; this.b=1; };
function Object3() { this.a=1; this.b=1; this.c=1; };
function Object4() { this.a=1; this.b=1; this.c=1; this.d=1; };
function Object5() { this.a=1; this.b=1; this.c=1; this.d=1; this.e=1; };

function test() {
    var N = 1e5;
    gc();

    for(var i = 0; i<=5; i++)
    {
        var tmp = i==0 ? Object0 : i==1 ? Object1 : i==2 ? Object2 : i==3 ? Object3 : i==4 ? Object4 : Object5;
        for (var j = 0; j != N; j++) {
            var a = new tmp();
        }
        gc();
    }
}

for(var i = 0; i<=5; i++) {
    test();
}
