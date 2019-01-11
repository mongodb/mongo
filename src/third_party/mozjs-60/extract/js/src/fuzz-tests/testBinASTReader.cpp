/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "mozilla/ScopeExit.h"

#include "jsapi.h"

#include "frontend/BinSource.h"
#include "frontend/FullParseHandler.h"
#include "frontend/ParseContext.h"
#include "frontend/Parser.h"
#include "fuzz-tests/tests.h"
#include "vm/Interpreter.h"

#include "vm/JSContext-inl.h"

using UsedNameTracker = js::frontend::UsedNameTracker;
using namespace js;

// These are defined and pre-initialized by the harness (in tests.cpp).
extern JS::PersistentRootedObject gGlobal;
extern JSContext* gCx;

static int
testBinASTReaderInit(int *argc, char ***argv) {
  return 0;
}

static int
testBinASTReaderFuzz(const uint8_t* buf, size_t size) {
    auto gcGuard = mozilla::MakeScopeExit([&] {
        JS::PrepareForFullGC(gCx);
        JS::GCForReason(gCx, GC_NORMAL, JS::gcreason::API);
    });

    if (!size) return 0;

    CompileOptions options(gCx);
    options.setIntroductionType("fuzzing parse")
       .setFileAndLine("<string>", 1);

    js::Vector<uint8_t> binSource(gCx);
    if (!binSource.append(buf, size)) {
        ReportOutOfMemory(gCx);
        return 0;
    }

    js::frontend::UsedNameTracker binUsedNames(gCx);
    if (!binUsedNames.init()) {
        ReportOutOfMemory(gCx);
        return 0;
    }

    js::frontend::BinASTParser reader(gCx, gCx->tempLifoAlloc(), binUsedNames, options);

    // Will be deallocated once `reader` goes out of scope.
    auto binParsed = reader.parse(binSource);
    RootedValue binExn(gCx);
    if (binParsed.isErr()) {
        js::GetAndClearException(gCx, &binExn);
        return 0;
    }

#if defined(DEBUG) // Dumping an AST is only defined in DEBUG builds
    Sprinter binPrinter(gCx);
    if (!binPrinter.init()) {
        ReportOutOfMemory(gCx);
        return 0;
    }
    DumpParseTree(binParsed.unwrap(), binPrinter);
#endif // defined(DEBUG)

    return 0;
}

MOZ_FUZZING_INTERFACE_RAW(testBinASTReaderInit, testBinASTReaderFuzz, BinAST);
