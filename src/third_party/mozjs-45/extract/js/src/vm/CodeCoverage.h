/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_CodeCoverage_h
#define vm_CodeCoverage_h

#include "mozilla/Vector.h"

#include "ds/LifoAlloc.h"

#include "vm/Printer.h"

struct JSCompartment;
class JSScript;
class JSObject;

namespace js {

class ScriptSourceObject;

namespace coverage {

class LCovCompartment;

class LCovSource
{
  public:
    explicit LCovSource(LifoAlloc* alloc, JSObject* sso);

    // Whether the given script source object matches this LCovSource.
    bool match(JSObject* sso) const {
        return sso == source_;
    }

    // Whether the current source is complete and if it can be flushed.
    bool isComplete() const {
        return hasFilename_ && hasTopLevelScript_;
    }

    // Iterate over the bytecode and collect the lcov output based on the
    // ScriptCounts counters.
    bool writeScript(JSScript* script);

    // Write the Lcov output in a buffer, such as the one associated with
    // the runtime code coverage trace file.
    void exportInto(GenericPrinter& out) const;

    // Write the script name in out.
    bool writeSourceFilename(ScriptSourceObject* sso);

  private:
    // Write the script name in out.
    bool writeScriptName(LSprinter& out, JSScript* script);

  private:
    // Weak pointer of the Script Source Object used by the current source.
    JSObject *source_;

    // LifoAlloc string which hold the filename of the source.
    LSprinter outSF_;

    // LifoAlloc strings which hold the filename of each function as
    // well as the number of hits for each function.
    LSprinter outFN_;
    LSprinter outFNDA_;
    size_t numFunctionsFound_;
    size_t numFunctionsHit_;

    // LifoAlloc string which hold branches statistics.
    LSprinter outBRDA_;
    size_t numBranchesFound_;
    size_t numBranchesHit_;

    // LifoAlloc string which hold lines statistics.
    LSprinter outDA_;
    size_t numLinesInstrumented_;
    size_t numLinesHit_;

    // Status flags.
    bool hasFilename_ : 1;
    bool hasTopLevelScript_ : 1;
};

class LCovCompartment
{
  public:
    LCovCompartment();

    // Collect code coverage information for the given source.
    void collectCodeCoverageInfo(JSCompartment* comp, JSObject* sso, JSScript* topLevel);

    // Create an ebtry for the current ScriptSourceObject.
    void collectSourceFile(JSCompartment* comp, ScriptSourceObject* sso);

    // Write the Lcov output in a buffer, such as the one associated with
    // the runtime code coverage trace file.
    void exportInto(GenericPrinter& out, bool* isEmpty) const;

  private:
    // Write the script name in out.
    bool writeCompartmentName(JSCompartment* comp);

    // Return the LCovSource entry which matches the given ScriptSourceObject.
    LCovSource* lookupOrAdd(JSCompartment* comp, JSObject* sso);

  private:
    typedef mozilla::Vector<LCovSource, 16, LifoAllocPolicy<Fallible>> LCovSourceVector;

    // LifoAlloc backend for all temporary allocations needed to stash the
    // strings to be written in the file.
    LifoAlloc alloc_;

    // LifoAlloc string which hold the name of the compartment.
    LSprinter outTN_;

    // Vector of all sources which are used in this compartment.
    LCovSourceVector* sources_;
};

class LCovRuntime
{
  public:
    LCovRuntime();
    ~LCovRuntime();

    // If the environment variable JS_CODE_COVERAGE_OUTPUT_DIR is set to a
    // directory, create a file inside this directory which uses the process
    // ID, the thread ID and a timestamp to ensure the uniqueness of the
    // file.
    //
    // At the end of the execution, this file should contains the LCOV output of
    // all the scripts executed in the current JSRuntime.
    void init();

    // Check if we should collect code coverage information.
    bool isEnabled() const { return out_.isInitialized(); }

    // Write the aggregated result of the code coverage of a compartment
    // into a file.
    void writeLCovResult(LCovCompartment& comp);

  private:
    // When a process forks, the file will remain open, but 2 processes will
    // have the same file. To avoid conflicting writes, we open a new file for
    // the child process.
    void maybeReopenAfterFork();

    // Fill an array with the name of the file. Return false if we are unable to
    // serialize the filename in this array.
    bool fillWithFilename(char *name, size_t length);

    // Finish the current opened file, and remove if it does not have any
    // content.
    void finishFile();

  private:
    // Output file which is created if code coverage is enabled.
    Fprinter out_;

    // The process' PID is used to watch for fork. When the process fork,
    // we want to close the current file and open a new one.
    size_t pid_;

    // Flag used to report if the generated file is empty or not. If it is empty
    // when the runtime is destroyed, then the file would be removed as an empty
    // file is not a valid LCov file.
    bool isEmpty_;
};

} // namespace coverage
} // namespace js

#endif // vm_Printer_h

