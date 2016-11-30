(function() {
    "use strict";

    const tooMuchRecursion = (1 << 16);

    const nestobj = (depth) => {
        let doc = {};
        let cur = doc;
        for (let i = 0; i < depth; i++) {
            cur[i] = {};
            cur = cur[i];
        }
        cur['a'] = 'foo';
        return doc;
    };

    const nestarr = (depth) => {
        let doc = [0];
        let cur = doc;
        for (let i = 0; i < depth; i++) {
            cur[0] = [0];
            cur = cur[0];
        }
        cur[0] = 'foo';
        return doc;
    };

    assert.doesNotThrow(
        tojson, [nestobj(tooMuchRecursion)], 'failed to print deeply nested object');
    assert.doesNotThrow(tojson, [nestarr(tooMuchRecursion)], 'failed to print deeply nested array');
})();
