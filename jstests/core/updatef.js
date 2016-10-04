// Test unsafe management of nsdt on update command yield SERVER-3208

prefixNS = db.jstests_updatef;
prefixNS.save({});

t = db.jstests_updatef_actual;
t.drop();

t.save({a: 0, b: []});
for (i = 0; i < 1000; ++i) {
    t.save({a: 100});
}
t.save({a: 0, b: []});

// Repeatedly rename jstests_updatef to jstests_updatef_ and back.  This will
// invalidate the jstests_updatef_actual NamespaceDetailsTransient object.
s = startParallelShell(
    "for( i=0; i < 100; ++i ) { db.jstests_updatef.renameCollection( 'jstests_updatef_' ); db.jstests_updatef_.renameCollection( 'jstests_updatef' ); }");

for (i = 0; i < 20; ++i) {
    t.update({a: 0}, {$push: {b: i}}, false, true);
}

s();
