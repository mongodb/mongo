/**
 * Tests that $projects with a large number of projected fields behave as expected.
 */
(function() {
const coll = db.large_project;
coll.drop();

const numProjects = 10000;
const doc = {
    _id: 15
};
for (let i = 0; i < numProjects; i++) {
    doc['a' + i] = i;
}
assert.commandWorked(coll.insert(doc));

{
    // Inclusion projection.
    const project = {};
    const expectedDoc = {_id: 15};
    for (let i = 0; i < numProjects; i++) {
        if (i % 2 === 1) {
            project['a' + i] = 1;
            expectedDoc['a' + i] = i;
        }
    }

    const result = coll.aggregate({$project: project}).toArray();
    assert.eq(result.length, 1);
    assert.docEq(result[0], expectedDoc);
}

{
    // Exclusion projection.
    const project = {};
    const expectedDoc = {_id: 15};
    for (let i = 0; i < numProjects; i++) {
        if (i % 2 === 0) {
            project['a' + i] = 0;
        } else {
            expectedDoc['a' + i] = i;
        }
    }

    const result = coll.aggregate({$project: project}).toArray();
    assert.eq(result.length, 1);
    assert.docEq(result[0], expectedDoc);
}
})();
