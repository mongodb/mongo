/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function StarGeneratorNext(val) {
    // The IsSuspendedStarGenerator call below is not necessary for
    // correctness. It's a performance optimization to check for the
    // common case with a single call. It's also inlined in Baseline.

    if (!IsSuspendedStarGenerator(this)) {
        if (!IsObject(this) || !IsStarGeneratorObject(this))
            return callFunction(CallStarGeneratorMethodIfWrapped, this, val, "StarGeneratorNext");

        if (StarGeneratorObjectIsClosed(this))
            return { value: undefined, done: true };

        if (GeneratorIsRunning(this))
            ThrowTypeError(JSMSG_NESTING_GENERATOR);
    }

    try {
        return resumeGenerator(this, val, 'next');
    } catch (e) {
        if (!StarGeneratorObjectIsClosed(this))
            GeneratorSetClosed(this);
        throw e;
    }
}

function StarGeneratorThrow(val) {
    if (!IsSuspendedStarGenerator(this)) {
        if (!IsObject(this) || !IsStarGeneratorObject(this))
            return callFunction(CallStarGeneratorMethodIfWrapped, this, val, "StarGeneratorThrow");

        if (StarGeneratorObjectIsClosed(this))
            throw val;

        if (GeneratorIsRunning(this))
            ThrowTypeError(JSMSG_NESTING_GENERATOR);
    }

    try {
        return resumeGenerator(this, val, 'throw');
    } catch (e) {
        if (!StarGeneratorObjectIsClosed(this))
            GeneratorSetClosed(this);
        throw e;
    }
}

function StarGeneratorReturn(val) {
    if (!IsSuspendedStarGenerator(this)) {
        if (!IsObject(this) || !IsStarGeneratorObject(this))
            return callFunction(CallStarGeneratorMethodIfWrapped, this, val, "StarGeneratorReturn");

        if (StarGeneratorObjectIsClosed(this))
            return { value: val, done: true };

        if (GeneratorIsRunning(this))
            ThrowTypeError(JSMSG_NESTING_GENERATOR);
    }

    try {
        var rval = { value: val, done: true };
        return resumeGenerator(this, rval, 'close');
    } catch (e) {
        if (!StarGeneratorObjectIsClosed(this))
            GeneratorSetClosed(this);
        throw e;
    }
}

function LegacyGeneratorNext(val) {
    if (!IsObject(this) || !IsLegacyGeneratorObject(this))
        return callFunction(CallLegacyGeneratorMethodIfWrapped, this, val, "LegacyGeneratorNext");

    if (LegacyGeneratorObjectIsClosed(this))
        ThrowStopIteration();

    if (GeneratorIsRunning(this))
        ThrowTypeError(JSMSG_NESTING_GENERATOR);

    try {
        return resumeGenerator(this, val, 'next');
    } catch(e) {
        if (!LegacyGeneratorObjectIsClosed(this))
            GeneratorSetClosed(this);
        throw e;
    }
}

function LegacyGeneratorThrow(val) {
    if (!IsObject(this) || !IsLegacyGeneratorObject(this))
        return callFunction(CallLegacyGeneratorMethodIfWrapped, this, val, "LegacyGeneratorThrow");

    if (LegacyGeneratorObjectIsClosed(this))
        throw val;

    if (GeneratorIsRunning(this))
        ThrowTypeError(JSMSG_NESTING_GENERATOR);

    try {
        return resumeGenerator(this, val, 'throw');
    } catch(e) {
        if (!LegacyGeneratorObjectIsClosed(this))
            GeneratorSetClosed(this);
        throw e;
    }
}

// Called by js::CloseIterator.
function LegacyGeneratorCloseInternal() {
    assert(IsObject(this), "Not an object: " + ToString(this));
    assert(IsLegacyGeneratorObject(this), "Not a legacy generator object: " + ToString(this));
    assert(!LegacyGeneratorObjectIsClosed(this), "Already closed: " + ToString(this));

    if (GeneratorIsRunning(this))
        ThrowTypeError(JSMSG_NESTING_GENERATOR);

    resumeGenerator(this, undefined, 'close');
    if (!LegacyGeneratorObjectIsClosed(this))
        CloseClosingLegacyGeneratorObject(this);
}

function LegacyGeneratorClose() {
    if (!IsObject(this) || !IsLegacyGeneratorObject(this))
        return callFunction(CallLegacyGeneratorMethodIfWrapped, this, "LegacyGeneratorClose");

    if (LegacyGeneratorObjectIsClosed(this))
        return;

    callFunction(LegacyGeneratorCloseInternal, this);
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
    assert(kind === "close", "Invalid resume kind");
    return resumeGenerator(gen, val, "close");
}
