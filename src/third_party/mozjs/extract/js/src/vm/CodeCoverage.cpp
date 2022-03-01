/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/CodeCoverage.h"

#include "mozilla/Atomics.h"
#include "mozilla/IntegerPrintfMacros.h"

#include <stdio.h>
#include <utility>

#include "frontend/SourceNotes.h"  // SrcNote, SrcNoteType, SrcNoteIterator
#include "gc/Zone.h"
#include "util/GetPidProvider.h"  // getpid()
#include "util/Text.h"
#include "vm/BytecodeUtil.h"
#include "vm/JSScript.h"
#include "vm/Realm.h"
#include "vm/Runtime.h"
#include "vm/Time.h"

// This file contains a few functions which are used to produce files understood
// by lcov tools. A detailed description of the format is available in the man
// page for "geninfo" [1].  To make it short, the following paraphrases what is
// commented in the man page by using curly braces prefixed by for-each to
// express repeated patterns.
//
//   TN:<compartment name>
//   for-each <source file> {
//     SN:<filename>
//     for-each <script> {
//       FN:<line>,<name>
//     }
//     for-each <script> {
//       FNDA:<hits>,<name>
//     }
//     FNF:<number of scripts>
//     FNH:<sum of scripts hits>
//     for-each <script> {
//       for-each <branch> {
//         BRDA:<line>,<block id>,<target id>,<taken>
//       }
//     }
//     BRF:<number of branches>
//     BRH:<sum of branches hits>
//     for-each <script> {
//       for-each <line> {
//         DA:<line>,<hits>
//       }
//     }
//     LF:<number of lines>
//     LH:<sum of lines hits>
//   }
//
// [1] http://ltp.sourceforge.net/coverage/lcov/geninfo.1.php
//
namespace js {
namespace coverage {

LCovSource::LCovSource(LifoAlloc* alloc, UniqueChars name)
    : name_(std::move(name)),
      outFN_(alloc),
      outFNDA_(alloc),
      numFunctionsFound_(0),
      numFunctionsHit_(0),
      outBRDA_(alloc),
      numBranchesFound_(0),
      numBranchesHit_(0),
      numLinesInstrumented_(0),
      numLinesHit_(0),
      maxLineHit_(0),
      hasTopLevelScript_(false),
      hadOOM_(false) {}

void LCovSource::exportInto(GenericPrinter& out) {
  if (hadOutOfMemory()) {
    out.reportOutOfMemory();
  } else {
    out.printf("SF:%s\n", name_.get());

    outFN_.exportInto(out);
    outFNDA_.exportInto(out);
    out.printf("FNF:%zu\n", numFunctionsFound_);
    out.printf("FNH:%zu\n", numFunctionsHit_);

    outBRDA_.exportInto(out);
    out.printf("BRF:%zu\n", numBranchesFound_);
    out.printf("BRH:%zu\n", numBranchesHit_);

    if (!linesHit_.empty()) {
      for (size_t lineno = 1; lineno <= maxLineHit_; ++lineno) {
        if (auto p = linesHit_.lookup(lineno)) {
          out.printf("DA:%zu,%" PRIu64 "\n", lineno, p->value());
        }
      }
    }

    out.printf("LF:%zu\n", numLinesInstrumented_);
    out.printf("LH:%zu\n", numLinesHit_);

    out.put("end_of_record\n");
  }

  outFN_.clear();
  outFNDA_.clear();
  numFunctionsFound_ = 0;
  numFunctionsHit_ = 0;
  outBRDA_.clear();
  numBranchesFound_ = 0;
  numBranchesHit_ = 0;
  linesHit_.clear();
  numLinesInstrumented_ = 0;
  numLinesHit_ = 0;
  maxLineHit_ = 0;
}

void LCovSource::writeScript(JSScript* script, const char* scriptName) {
  if (hadOutOfMemory()) {
    return;
  }

  numFunctionsFound_++;
  outFN_.printf("FN:%u,%s\n", script->lineno(), scriptName);

  uint64_t hits = 0;
  ScriptCounts* sc = nullptr;
  if (script->hasScriptCounts()) {
    sc = &script->getScriptCounts();
    numFunctionsHit_++;
    const PCCounts* counts =
        sc->maybeGetPCCounts(script->pcToOffset(script->main()));
    outFNDA_.printf("FNDA:%" PRIu64 ",%s\n", counts->numExec(), scriptName);

    // Set the hit count of the pre-main code to 1, if the function ever got
    // visited.
    hits = 1;
  }

  jsbytecode* snpc = script->code();
  const SrcNote* sn = script->notes();
  if (!sn->isTerminator()) {
    snpc += sn->delta();
  }

  size_t lineno = script->lineno();
  jsbytecode* end = script->codeEnd();
  size_t branchId = 0;
  bool firstLineHasBeenWritten = false;
  for (jsbytecode* pc = script->code(); pc != end; pc = GetNextPc(pc)) {
    MOZ_ASSERT(script->code() <= pc && pc < end);
    JSOp op = JSOp(*pc);
    bool jump = IsJumpOpcode(op) || op == JSOp::TableSwitch;
    bool fallsthrough = BytecodeFallsThrough(op) && op != JSOp::Gosub;

    // If the current script & pc has a hit-count report, then update the
    // current number of hits.
    if (sc) {
      const PCCounts* counts = sc->maybeGetPCCounts(script->pcToOffset(pc));
      if (counts) {
        hits = counts->numExec();
      }
    }

    // If we have additional source notes, walk all the source notes of the
    // current pc.
    if (snpc <= pc || !firstLineHasBeenWritten) {
      size_t oldLine = lineno;
      SrcNoteIterator iter(sn);
      while (!iter.atEnd() && snpc <= pc) {
        sn = *iter;
        SrcNoteType type = sn->type();
        if (type == SrcNoteType::SetLine) {
          lineno = SrcNote::SetLine::getLine(sn, script->lineno());
        } else if (type == SrcNoteType::NewLine) {
          lineno++;
        }
        ++iter;
        snpc += (*iter)->delta();
      }
      sn = *iter;

      if ((oldLine != lineno || !firstLineHasBeenWritten) &&
          pc >= script->main() && fallsthrough) {
        auto p = linesHit_.lookupForAdd(lineno);
        if (!p) {
          if (!linesHit_.add(p, lineno, hits)) {
            hadOOM_ = true;
            return;
          }
          numLinesInstrumented_++;
          if (hits != 0) {
            numLinesHit_++;
          }
          maxLineHit_ = std::max(lineno, maxLineHit_);
        } else {
          if (p->value() == 0 && hits != 0) {
            numLinesHit_++;
          }
          p->value() += hits;
        }

        firstLineHasBeenWritten = true;
      }
    }

    // If the current instruction has thrown, then decrement the hit counts
    // with the number of throws.
    if (sc) {
      const PCCounts* counts = sc->maybeGetThrowCounts(script->pcToOffset(pc));
      if (counts) {
        hits -= counts->numExec();
      }
    }

    // If the current pc corresponds to a conditional jump instruction, then
    // reports branch hits.
    if (jump && fallsthrough) {
      jsbytecode* fallthroughTarget = GetNextPc(pc);
      uint64_t fallthroughHits = 0;
      if (sc) {
        const PCCounts* counts =
            sc->maybeGetPCCounts(script->pcToOffset(fallthroughTarget));
        if (counts) {
          fallthroughHits = counts->numExec();
        }
      }

      uint64_t taken = hits - fallthroughHits;
      outBRDA_.printf("BRDA:%zu,%zu,0,", lineno, branchId);
      if (hits) {
        outBRDA_.printf("%" PRIu64 "\n", taken);
      } else {
        outBRDA_.put("-\n", 2);
      }

      outBRDA_.printf("BRDA:%zu,%zu,1,", lineno, branchId);
      if (hits) {
        outBRDA_.printf("%" PRIu64 "\n", fallthroughHits);
      } else {
        outBRDA_.put("-\n", 2);
      }

      // Count the number of branches, and the number of branches hit.
      numBranchesFound_ += 2;
      if (hits) {
        numBranchesHit_ += !!taken + !!fallthroughHits;
      }
      branchId++;
    }

    // If the current pc corresponds to a pre-computed switch case, then
    // reports branch hits for each case statement.
    if (jump && op == JSOp::TableSwitch) {
      // Get the default pc.
      jsbytecode* defaultpc = pc + GET_JUMP_OFFSET(pc);
      MOZ_ASSERT(script->code() <= defaultpc && defaultpc < end);
      MOZ_ASSERT(defaultpc > pc);

      // Get the low and high from the tableswitch
      int32_t low = GET_JUMP_OFFSET(pc + JUMP_OFFSET_LEN * 1);
      int32_t high = GET_JUMP_OFFSET(pc + JUMP_OFFSET_LEN * 2);
      MOZ_ASSERT(high - low + 1 >= 0);
      size_t numCases = high - low + 1;

      auto getCaseOrDefaultPc = [&](size_t index) {
        if (index < numCases) {
          return script->tableSwitchCasePC(pc, index);
        }
        MOZ_ASSERT(index == numCases);
        return defaultpc;
      };

      jsbytecode* firstCaseOrDefaultPc = end;
      for (size_t j = 0; j < numCases + 1; j++) {
        jsbytecode* testpc = getCaseOrDefaultPc(j);
        MOZ_ASSERT(script->code() <= testpc && testpc < end);
        if (testpc < firstCaseOrDefaultPc) {
          firstCaseOrDefaultPc = testpc;
        }
      }

      // Count the number of hits of the default branch, by subtracting
      // the number of hits of each cases.
      uint64_t defaultHits = hits;

      // Count the number of hits of the previous case entry.
      uint64_t fallsThroughHits = 0;

      // Record branches for each case and default.
      size_t caseId = 0;
      for (size_t i = 0; i < numCases + 1; i++) {
        jsbytecode* caseOrDefaultPc = getCaseOrDefaultPc(i);
        MOZ_ASSERT(script->code() <= caseOrDefaultPc && caseOrDefaultPc < end);

        // PCs might not be in increasing order of case indexes.
        jsbytecode* lastCaseOrDefaultPc = firstCaseOrDefaultPc - 1;
        bool foundLastCaseOrDefault = false;
        for (size_t j = 0; j < numCases + 1; j++) {
          jsbytecode* testpc = getCaseOrDefaultPc(j);
          MOZ_ASSERT(script->code() <= testpc && testpc < end);
          if (lastCaseOrDefaultPc < testpc &&
              (testpc < caseOrDefaultPc ||
               (j < i && testpc == caseOrDefaultPc))) {
            lastCaseOrDefaultPc = testpc;
            foundLastCaseOrDefault = true;
          }
        }

        // If multiple case instruction have the same code block, only
        // register the code coverage the first time we hit this case.
        if (!foundLastCaseOrDefault || caseOrDefaultPc != lastCaseOrDefaultPc) {
          uint64_t caseOrDefaultHits = 0;
          if (sc) {
            if (i < numCases) {
              // Case (i + low)
              const PCCounts* counts =
                  sc->maybeGetPCCounts(script->pcToOffset(caseOrDefaultPc));
              if (counts) {
                caseOrDefaultHits = counts->numExec();
              }

              // Remove fallthrough.
              fallsThroughHits = 0;
              if (foundLastCaseOrDefault) {
                // Walk from the previous case to the current one to
                // check if it fallthrough into the current block.
                MOZ_ASSERT(lastCaseOrDefaultPc != firstCaseOrDefaultPc - 1);
                jsbytecode* endpc = lastCaseOrDefaultPc;
                while (GetNextPc(endpc) < caseOrDefaultPc) {
                  endpc = GetNextPc(endpc);
                  MOZ_ASSERT(script->code() <= endpc && endpc < end);
                }

                if (BytecodeFallsThrough(JSOp(*endpc))) {
                  fallsThroughHits = script->getHitCount(endpc);
                }
              }
              caseOrDefaultHits -= fallsThroughHits;
            } else {
              caseOrDefaultHits = defaultHits;
            }
          }

          outBRDA_.printf("BRDA:%zu,%zu,%zu,", lineno, branchId, caseId);
          if (hits) {
            outBRDA_.printf("%" PRIu64 "\n", caseOrDefaultHits);
          } else {
            outBRDA_.put("-\n", 2);
          }

          numBranchesFound_++;
          numBranchesHit_ += !!caseOrDefaultHits;
          if (i < numCases) {
            defaultHits -= caseOrDefaultHits;
          }
          caseId++;
        }
      }
    }
  }

  if (outFN_.hadOutOfMemory() || outFNDA_.hadOutOfMemory() ||
      outBRDA_.hadOutOfMemory()) {
    hadOOM_ = true;
    return;
  }

  // If this script is the top-level script, then record it such that we can
  // assume that the code coverage report is complete, as this script has
  // references on all inner scripts.
  if (script->isTopLevel()) {
    hasTopLevelScript_ = true;
  }
}

LCovRealm::LCovRealm(JS::Realm* realm)
    : alloc_(4096), outTN_(&alloc_), sources_(alloc_) {
  // Record realm name. If we wait until finalization, the embedding may not be
  // able to provide us the name anymore.
  writeRealmName(realm);
}

LCovRealm::~LCovRealm() {
  // The LCovSource are in the LifoAlloc but we must still manually invoke
  // destructors to avoid leaks.
  while (!sources_.empty()) {
    LCovSource* source = sources_.popCopy();
    source->~LCovSource();
  }
}

LCovSource* LCovRealm::lookupOrAdd(const char* name) {
  // Find existing source if it exists.
  for (LCovSource* source : sources_) {
    if (source->match(name)) {
      return source;
    }
  }

  UniqueChars source_name = DuplicateString(name);
  if (!source_name) {
    outTN_.reportOutOfMemory();
    return nullptr;
  }

  // Allocate a new LCovSource for the current top-level.
  LCovSource* source = alloc_.new_<LCovSource>(&alloc_, std::move(source_name));
  if (!source) {
    outTN_.reportOutOfMemory();
    return nullptr;
  }

  if (!sources_.emplaceBack(source)) {
    outTN_.reportOutOfMemory();
    return nullptr;
  }

  return source;
}

void LCovRealm::exportInto(GenericPrinter& out, bool* isEmpty) const {
  if (outTN_.hadOutOfMemory()) {
    return;
  }

  // If we only have cloned function, then do not serialize anything.
  bool someComplete = false;
  for (const LCovSource* sc : sources_) {
    if (sc->isComplete()) {
      someComplete = true;
      break;
    };
  }

  if (!someComplete) {
    return;
  }

  *isEmpty = false;
  outTN_.exportInto(out);
  for (LCovSource* sc : sources_) {
    // Only write if everything got recorded.
    if (sc->isComplete()) {
      sc->exportInto(out);
    }
  }
}

void LCovRealm::writeRealmName(JS::Realm* realm) {
  JSContext* cx = TlsContext.get();

  // lcov trace files are starting with an optional test case name, that we
  // recycle to be a realm name.
  //
  // Note: The test case name has some constraint in terms of valid character,
  // thus we escape invalid chracters with a "_" symbol in front of its
  // hexadecimal code.
  outTN_.put("TN:");
  if (cx->runtime()->realmNameCallback) {
    char name[1024];
    {
      // Hazard analysis cannot tell that the callback does not GC.
      JS::AutoSuppressGCAnalysis nogc;
      (*cx->runtime()->realmNameCallback)(cx, realm, name, sizeof(name), nogc);
    }
    for (char* s = name; s < name + sizeof(name) && *s; s++) {
      if (('a' <= *s && *s <= 'z') || ('A' <= *s && *s <= 'Z') ||
          ('0' <= *s && *s <= '9')) {
        outTN_.put(s, 1);
        continue;
      }
      outTN_.printf("_%p", (void*)size_t(*s));
    }
    outTN_.put("\n", 1);
  } else {
    outTN_.printf("Realm_%p%p\n", (void*)size_t('_'), realm);
  }
}

const char* LCovRealm::getScriptName(JSScript* script) {
  JSFunction* fun = script->function();
  if (fun && fun->displayAtom()) {
    JSAtom* atom = fun->displayAtom();
    size_t lenWithNull = js::PutEscapedString(nullptr, 0, atom, 0) + 1;
    char* name = alloc_.newArray<char>(lenWithNull);
    if (name) {
      js::PutEscapedString(name, lenWithNull, atom, 0);
    }
    return name;
  }
  return "top-level";
}

bool gLCovIsEnabled = false;

void InitLCov() {
  const char* outDir = getenv("JS_CODE_COVERAGE_OUTPUT_DIR");
  if (outDir && *outDir != 0) {
    EnableLCov();
  }
}

void EnableLCov() {
  MOZ_ASSERT(!JSRuntime::hasLiveRuntimes(),
             "EnableLCov must not be called after creating a runtime!");
  gLCovIsEnabled = true;
}

LCovRuntime::LCovRuntime() : out_(), pid_(getpid()), isEmpty_(true) {}

LCovRuntime::~LCovRuntime() {
  if (out_.isInitialized()) {
    finishFile();
  }
}

bool LCovRuntime::fillWithFilename(char* name, size_t length) {
  const char* outDir = getenv("JS_CODE_COVERAGE_OUTPUT_DIR");
  if (!outDir || *outDir == 0) {
    return false;
  }

  int64_t timestamp = static_cast<double>(PRMJ_Now()) / PRMJ_USEC_PER_SEC;
  static mozilla::Atomic<size_t> globalRuntimeId(0);
  size_t rid = globalRuntimeId++;

  int len = snprintf(name, length, "%s/%" PRId64 "-%" PRIu32 "-%zu.info",
                     outDir, timestamp, pid_, rid);
  if (len < 0 || size_t(len) >= length) {
    fprintf(stderr,
            "Warning: LCovRuntime::init: Cannot serialize file name.\n");
    return false;
  }

  return true;
}

void LCovRuntime::init() {
  char name[1024];
  if (!fillWithFilename(name, sizeof(name))) {
    return;
  }

  // If we cannot open the file, report a warning.
  if (!out_.init(name)) {
    fprintf(stderr,
            "Warning: LCovRuntime::init: Cannot open file named '%s'.\n", name);
  }
  isEmpty_ = true;
}

void LCovRuntime::finishFile() {
  MOZ_ASSERT(out_.isInitialized());
  out_.finish();

  if (isEmpty_) {
    char name[1024];
    if (!fillWithFilename(name, sizeof(name))) {
      return;
    }
    remove(name);
  }
}

void LCovRuntime::writeLCovResult(LCovRealm& realm) {
  if (!out_.isInitialized()) {
    init();
    if (!out_.isInitialized()) {
      return;
    }
  }

  uint32_t p = getpid();
  if (pid_ != p) {
    pid_ = p;
    finishFile();
    init();
    if (!out_.isInitialized()) {
      return;
    }
  }

  realm.exportInto(out_, &isEmpty_);
  out_.flush();
  finishFile();
}

bool InitScriptCoverage(JSContext* cx, JSScript* script) {
  MOZ_ASSERT(IsLCovEnabled());
  MOZ_ASSERT(script->hasBytecode(),
             "Only initialize coverage data for fully initialized scripts.");

  // Don't allocate LCovSource if we on helper thread since we will have our
  // realm migrated. The 'GCRunime::mergeRealms' code will do this
  // initialization.
  if (cx->isHelperThreadContext()) {
    return true;
  }

  const char* filename = script->filename();
  if (!filename) {
    return true;
  }

  // Create LCovRealm if necessary.
  LCovRealm* lcovRealm = script->realm()->lcovRealm();
  if (!lcovRealm) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Create LCovSource if necessary.
  LCovSource* source = lcovRealm->lookupOrAdd(filename);
  if (!source) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Computed the formated script name.
  const char* scriptName = lcovRealm->getScriptName(script);
  if (!scriptName) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Create Zone::scriptLCovMap if necessary.
  JS::Zone* zone = script->zone();
  if (!zone->scriptLCovMap) {
    zone->scriptLCovMap = cx->make_unique<ScriptLCovMap>();
  }
  if (!zone->scriptLCovMap) {
    return false;
  }

  MOZ_ASSERT(script->hasBytecode());

  // Save source in map for when we collect coverage.
  if (!zone->scriptLCovMap->putNew(script,
                                   mozilla::MakeTuple(source, scriptName))) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

bool CollectScriptCoverage(JSScript* script, bool finalizing) {
  MOZ_ASSERT(IsLCovEnabled());

  ScriptLCovMap* map = script->zone()->scriptLCovMap.get();
  if (!map) {
    return false;
  }

  auto p = map->lookup(script);
  if (!p.found()) {
    return false;
  }

  LCovSource* source;
  const char* scriptName;
  mozilla::Tie(source, scriptName) = p->value();

  if (script->hasBytecode()) {
    source->writeScript(script, scriptName);
  }

  if (finalizing) {
    map->remove(p);
  }

  // Propagate the failure in case caller wants to terminate early.
  return !source->hadOutOfMemory();
}

}  // namespace coverage
}  // namespace js
