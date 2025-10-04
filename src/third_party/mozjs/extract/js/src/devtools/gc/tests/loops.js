//Measure plain GC.

var t = [];
var N = 500000

for(var i = 0; i < N; i++)
    t[i] = {};

gc()

t = [];

gc();

for(var i = 0; i < N; i++)
    t[i] = ({});

gc();

t = [];

gc();


for(var i = 0; i < N; i++)
    t[i] = "asdf";
    
gc();

t = [];

gc();


for(var i = 0; i < N; i++)
    t[i] = 1.12345;
    
gc();

t=[];

gc();

for(var i = 0; i < N; i++) {
    t[i] = ({});
    if (i != 0)
        t[i].a = t[i-1];
}
    
gc();

t = [];

gc();

