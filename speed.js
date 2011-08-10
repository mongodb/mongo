t = db.fooo;
t.drop();
x = { str:'aaaabbbbcc' }
s = new Date();
for( var i = 0; i < 100000; i++ ) { 
    x.i = i;
    t.insert(x);
}
print( (new Date())-s );
t.ensureIndex({x:1});
t.ensureIndex({str:1});
print( (new Date())-s );

