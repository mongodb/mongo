
import {describe, it, runTests} from "jstests/libs/mochalite.js";

describe("Set shims and polyfills", function() {
    it("tojson", function() {
        const s0 = new Set();

        assert.eq(Set.tojson(s0), 'new Set([ ])');

        const s = new Set(["value1", "value2", "value2", 2, 2.0, "2"]);
        assert.eq(Set.tojson(s, '', true), 'new Set([ "value1", "value2", 2, "2" ])');

        assert.eq(toJsonForLog(s), '{"$set":["value1","value2",2,"2"]}');
    });
});

runTests();
