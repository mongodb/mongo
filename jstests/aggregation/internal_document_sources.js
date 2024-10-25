// This is intended to reproduce SERVER-95731. The $_internal* document sources are not intended to
// be user-facing. This test verifies use using internal document sources fails.

const coll = db.internal_document_source;
assert.commandWorked(coll.insert({a: 1}));

const pipelines = [
    [{$_internalReplaceRoot: {}}],
    [{$_internalProjection: {}}],
];

for (const pipeline of pipelines) {
    assert.throwsWithCode(() => coll.aggregate(pipeline), 40324);
}
