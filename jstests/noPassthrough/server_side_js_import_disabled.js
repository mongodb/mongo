/**
 * Module loading is a shell-only feature and must be disabled in the server execution environment.
 * Server-side JavaScript (e.g. $function) must not be able to load or execute on-disk ES modules via
 * import() -- otherwise it could read arbitrary host files or run arbitrary on-disk JavaScript.
 *
 * @tags: [requires_scripting]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("server-side JavaScript cannot load modules via import()", function () {
    // A module whose top-level code, if it were ever loaded and evaluated, leaks a recognizable
    // token onto the global object. Module evaluation runs synchronously during import(), so on a
    // server where module loading was enabled this side effect would be observable; with module
    // loading disabled the file is never read or executed and the token never appears.
    const sideEffectToken = "module_executed_" + new Date().getTime();
    const modulePath = MongoRunner.dataPath + "server_side_import_module.mjs";

    before(function () {
        this.conn = MongoRunner.runMongod({});
        assert.neq(null, this.conn, "mongod failed to start");
        this.db = this.conn.getDB("test");
        writeFile(modulePath, `globalThis.__importSideEffect = '${sideEffectToken}';\n`);
    });

    after(function () {
        if (this.conn) {
            MongoRunner.stopMongod(this.conn);
        }
        removeFile(modulePath);
    });

    it("runs ordinary server-side $function (baseline)", function () {
        const res = this.db
            .aggregate([
                {$documents: [{i: 1}]},
                {
                    $project: {
                        _id: 0,
                        out: {
                            $function: {
                                lang: "js",
                                args: ["$i"],
                                body: "function(i) { return i + 1; }",
                            },
                        },
                    },
                },
            ])
            .toArray();
        assert.eq(2, res[0].out, "baseline server-side $function should run");
    });

    it("does not load or execute an on-disk module via import()", function () {
        const body = `function(p) {
            try {
                import(p);
            } catch (e) {
                // A synchronous failure is acceptable; we only care that the module never executed.
            }
            return (typeof globalThis.__importSideEffect === 'string')
                ? globalThis.__importSideEffect
                : 'not-executed';
        }`;
        const res = this.db
            .aggregate([
                {$documents: [{}]},
                {$project: {_id: 0, out: {$function: {lang: "js", args: [modulePath], body}}}},
            ])
            .toArray();
        assert.eq("not-executed", res[0].out, "import() loaded/executed an on-disk module on the server");
        assert(!tojson(res).includes(sideEffectToken), "on-disk module side effect leaked into the server", {res});
    });
});
