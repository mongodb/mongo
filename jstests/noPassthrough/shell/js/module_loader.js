import {describe, it} from "jstests/libs/mochalite.js";

describe("module loader internal binding restrictions", function () {
    it("does not allow scripts to import internal bindings as ES modules", async function () {
        let importError = null;
        try {
            await import("performance");
        } catch (error) {
            importError = error;
        }

        assert.neq(importError, null, "scripts should not import internal bindings as ES modules");
    });

    it("prevents non-std modules from calling internalModule()", function () {
        let callError = assert.throws(() => internalModule("performance"));
        assert.neq(callError, null, "non-std modules should not call internalModule()");
        assert(
            callError.message.includes("restricted to std:* modules"),
            `unexpected internalModule error: ${callError.message}`,
        );
    });
});
