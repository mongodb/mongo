/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function GeneratorNext(val) {
    // The IsSuspendedGenerator call below is not necessary for correctness.
    // It's a performance optimization to check for the common case with a
    // single call. It's also inlined in Baseline.

    if (!IsSuspendedGenerator(this)) {
        if (!IsObject(this) || !IsGeneratorObject(this))
            return callFunction(CallGeneratorMethodIfWrapped, this, val, "GeneratorNext");

        if (GeneratorObjectIsClosed(this))
            return { value: undefined, done: true };

        if (GeneratorIsRunning(this))
            ThrowTypeError(JSMSG_NESTING_GENERATOR);
    }

    try {
        return resumeGenerator(this, val, "next");
    } catch (e) {
        if (!GeneratorObjectIsClosed(this))
            GeneratorSetClosed(this);
        throw e;
    }
}

function GeneratorThrow(val) {
    if (!IsSuspendedGenerator(this)) {
        if (!IsObject(this) || !IsGeneratorObject(this))
            return callFunction(CallGeneratorMethodIfWrapped, this, val, "GeneratorThrow");

        if (GeneratorObjectIsClosed(this))
            throw val;

        if (GeneratorIsRunning(this))
            ThrowTypeError(JSMSG_NESTING_GENERATOR);
    }

    try {
        return resumeGenerator(this, val, "throw");
    } catch (e) {
        if (!GeneratorObjectIsClosed(this))
            GeneratorSetClosed(this);
        throw e;
    }
}

function GeneratorReturn(val) {
    if (!IsSuspendedGenerator(this)) {
        if (!IsObject(this) || !IsGeneratorObject(this))
            return callFunction(CallGeneratorMethodIfWrapped, this, val, "GeneratorReturn");

        if (GeneratorObjectIsClosed(this))
            return { value: val, done: true };

        if (GeneratorIsRunning(this))
            ThrowTypeError(JSMSG_NESTING_GENERATOR);
    }

    try {
        var rval = { value: val, done: true };
        return resumeGenerator(this, rval, "return");
    } catch (e) {
        if (!GeneratorObjectIsClosed(this))
            GeneratorSetClosed(this);
        throw e;
    }
}

function InterpretGeneratorResume(gen, val, kind) {
    // If we want to resume a generator in the interpreter, the script containing
    // the resumeGenerator/JSOP_RESUME also has to run in the interpreter. The
    // forceInterpreter() call below compiles to a bytecode op that prevents us
    // from JITing this script.
    forceInterpreter();
    if (kind === "next")
       return resumeGenerator(gen, val, "next");
    if (kind === "throw")
       return resumeGenerator(gen, val, "throw");
    assert(kind === "return", "Invalid resume kind");
    return resumeGenerator(gen, val, "return");
}
