/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_CodeCoverage_h
#define vm_CodeCoverage_h

#include "mozilla/Vector.h"

#include "ds/LifoAlloc.h"

#include "js/AllocPolicy.h"
#include "js/HashTable.h"
#include "js/Printer.h"
#include "js/TypeDecls.h"
#include "js/Utility.h"

namespace js {
namespace coverage {

class LCovSource {
 public:
  LCovSource(LifoAlloc* alloc, JS::UniqueChars name);

  // Whether the given script name matches this LCovSource.
  bool match(const char* name) const { return strcmp(name_.get(), name) == 0; }

  // Whether an OOM was seen recording coverage information. This indicates
  // that the resulting coverage information is incomplete.
  bool hadOutOfMemory() const { return hadOOM_; }

  // Whether the current source is complete and if it can be flushed.
  bool isComplete() const { return hasTopLevelScript_; }

  // Iterate over the bytecode and collect the lcov output based on the
  // ScriptCounts counters.
  void writeScript(JSScript* script, const char* scriptName);

  // Write the Lcov output in a buffer, such as the one associated with
  // the runtime code coverage trace file.
  void exportInto(GenericPrinter& out);

 private:
  // Name of the source file.
  JS::UniqueChars name_;

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

  // Holds lines statistics. When processing a line hit count, the hit count
  // is added to any hit count already in the hash map so that we handle
  // lines that belong to more than one JSScript or function in the same
  // source file.
  HashMap<size_t, uint64_t, DefaultHasher<size_t>, SystemAllocPolicy> linesHit_;
  size_t numLinesInstrumented_;
  size_t numLinesHit_;
  size_t maxLineHit_;

  // Status flags.
  bool hasTopLevelScript_ : 1;
  bool hadOOM_ : 1;
};

class LCovRealm {
 public:
  explicit LCovRealm(JS::Realm* realm);
  ~LCovRealm();

  // Write the Lcov output in a buffer, such as the one associated with
  // the runtime code coverage trace file.
  void exportInto(GenericPrinter& out, bool* isEmpty) const;

  friend bool InitScriptCoverage(JSContext* cx, JSScript* script);

 private:
  // Write the realm name in outTN_.
  void writeRealmName(JS::Realm* realm);

  // Return the LCovSource entry which matches the given ScriptSourceObject.
  LCovSource* lookupOrAdd(const char* name);

  // Generate escaped form of script atom and allocate inside our LifoAlloc if
  // necessary.
  const char* getScriptName(JSScript* script);

 private:
  typedef mozilla::Vector<LCovSource*, 16, LifoAllocPolicy<Fallible>>
      LCovSourceVector;

  // LifoAlloc backend for all temporary allocations needed to stash the
  // strings to be written in the file.
  LifoAlloc alloc_;

  // LifoAlloc string which hold the name of the realm.
  LSprinter outTN_;

  // Vector of all sources which are used in this realm. The entries are
  // allocated within the LifoAlloc.
  LCovSourceVector sources_;
};

class LCovRuntime {
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

  // Write the aggregated result of the code coverage of a realm
  // into a file.
  void writeLCovResult(LCovRealm& realm);

 private:
  // Fill an array with the name of the file. Return false if we are unable to
  // serialize the filename in this array.
  bool fillWithFilename(char* name, size_t length);

  // Finish the current opened file, and remove if it does not have any
  // content.
  void finishFile();

 private:
  // Output file which is created if code coverage is enabled.
  Fprinter out_;

  // The process' PID is used to watch for fork. When the process fork,
  // we want to close the current file and open a new one.
  uint32_t pid_;

  // Flag used to report if the generated file is empty or not. If it is empty
  // when the runtime is destroyed, then the file would be removed as an empty
  // file is not a valid LCov file.
  bool isEmpty_;
};

void InitLCov();

void EnableLCov();

inline bool IsLCovEnabled() {
  extern bool gLCovIsEnabled;
  return gLCovIsEnabled;
}

// Initialize coverage info to track code coverage for a JSScript.
bool InitScriptCoverage(JSContext* cx, JSScript* script);

// Collect the code-coverage data from a script into relevant LCovSource.
bool CollectScriptCoverage(JSScript* script, bool finalizing);

}  // namespace coverage
}  // namespace js

#endif  // vm_Printer_h
