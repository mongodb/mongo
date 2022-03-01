/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifdef JS_CACHEIR_SPEW

#  include "jit/CacheIRHealth.h"

#  include "mozilla/Maybe.h"

#  include "gc/Zone.h"
#  include "jit/CacheIRCompiler.h"
#  include "jit/JitScript.h"

using namespace js;
using namespace js::jit;

// TODO: Refine how we assign happiness based on total health score.
CacheIRHealth::Happiness CacheIRHealth::determineStubHappiness(
    uint32_t stubHealthScore) {
  if (stubHealthScore >= 30) {
    return Sad;
  }

  if (stubHealthScore >= 20) {
    return MediumSad;
  }

  if (stubHealthScore >= 10) {
    return MediumHappy;
  }

  return Happy;
}

CacheIRHealth::Happiness CacheIRHealth::spewStubHealth(
    AutoStructuredSpewer& spew, ICCacheIRStub* stub) {
  const CacheIRStubInfo* stubInfo = stub->stubInfo();
  CacheIRReader stubReader(stubInfo);
  uint32_t totalStubHealth = 0;

  spew->beginListProperty("cacheIROps");
  while (stubReader.more()) {
    CacheOp op = stubReader.readOp();
    uint32_t opHealth = CacheIROpHealth[size_t(op)];
    uint32_t argLength = CacheIROpInfos[size_t(op)].argLength;
    const char* opName = CacheIROpNames[size_t(op)];

    spew->beginObject();
    if (opHealth == UINT32_MAX) {
      spew->property("unscoredOp", opName);
    } else {
      spew->property("cacheIROp", opName);
      spew->property("opHealth", opHealth);
      totalStubHealth += opHealth;
    }
    spew->endObject();

    stubReader.skip(argLength);
  }
  spew->endList();  // cacheIROps

  spew->property("stubHealth", totalStubHealth);

  Happiness stubHappiness = determineStubHappiness(totalStubHealth);
  spew->property("stubHappiness", stubHappiness);

  return stubHappiness;
}

bool CacheIRHealth::spewNonFallbackICInformation(AutoStructuredSpewer& spew,
                                                 ICStub* firstStub,
                                                 Happiness* entryHappiness) {
  const CacheIRStubInfo* stubInfo = firstStub->toCacheIRStub()->stubInfo();
  Vector<bool, 8, SystemAllocPolicy> sawDistinctValueAtFieldIndex;

  bool sawNonZeroCount = false;
  bool sawDifferentCacheIRStubs = false;
  ICStub* stub = firstStub;

  spew->beginListProperty("stubs");
  while (stub && !stub->isFallback()) {
    spew->beginObject();
    {
      Happiness stubHappiness = spewStubHealth(spew, stub->toCacheIRStub());
      if (stubHappiness < *entryHappiness) {
        *entryHappiness = stubHappiness;
      }

      ICStub* nextStub = stub->toCacheIRStub()->next();
      if (!nextStub->isFallback()) {
        if (nextStub->enteredCount() > 0) {
          // More than one stub has a hit count greater than zero.
          // This is sad because we do not Warp transpile in this case.
          *entryHappiness = Sad;
          sawNonZeroCount = true;
        }

        if (nextStub->toCacheIRStub()->stubInfo() != stubInfo) {
          sawDifferentCacheIRStubs = true;
        }

        // If there are multiple stubs with non zero hit counts and if all
        // of the stubs have equivalent CacheIR, then keep track of how many
        // distinct stub field values are seen for each field index.
        if (sawNonZeroCount && !sawDifferentCacheIRStubs) {
          uint32_t fieldIndex = 0;
          size_t offset = 0;

          while (stubInfo->fieldType(fieldIndex) != StubField::Type::Limit) {
            if (sawDistinctValueAtFieldIndex.length() <= fieldIndex) {
              if (!sawDistinctValueAtFieldIndex.append(false)) {
                return false;
              }
            }

            if (StubField::sizeIsWord(stubInfo->fieldType(fieldIndex))) {
              uintptr_t firstRaw =
                  stubInfo->getStubRawWord(firstStub->toCacheIRStub(), offset);
              uintptr_t nextRaw =
                  stubInfo->getStubRawWord(nextStub->toCacheIRStub(), offset);
              if (firstRaw != nextRaw) {
                sawDistinctValueAtFieldIndex[fieldIndex] = true;
              }
            } else {
              MOZ_ASSERT(
                  StubField::sizeIsInt64(stubInfo->fieldType(fieldIndex)));
              int64_t firstRaw =
                  stubInfo->getStubRawInt64(firstStub->toCacheIRStub(), offset);
              int64_t nextRaw =
                  stubInfo->getStubRawInt64(nextStub->toCacheIRStub(), offset);

              if (firstRaw != nextRaw) {
                sawDistinctValueAtFieldIndex[fieldIndex] = true;
              }
            }

            offset += StubField::sizeInBytes(stubInfo->fieldType(fieldIndex));
            fieldIndex++;
          }
        }
      }

      spew->property("hitCount", stub->enteredCount());
      stub = nextStub;
    }
    spew->endObject();
  }
  spew->endList();  // stubs

  // If more than one CacheIR stub has an entered count greater than
  // zero and all the stubs have equivalent CacheIR, then spew
  // the information collected about the stub fields across the IC.
  if (sawNonZeroCount && !sawDifferentCacheIRStubs) {
    spew->beginListProperty("stubFields");
    for (size_t i = 0; i < sawDistinctValueAtFieldIndex.length(); i++) {
      spew->beginObject();
      {
        spew->property("fieldType", uint8_t(stubInfo->fieldType(i)));
        spew->property("sawDistinctFieldValues",
                       sawDistinctValueAtFieldIndex[i]);
      }
      spew->endObject();
    }
    spew->endList();
  }

  return true;
}

bool CacheIRHealth::spewICEntryHealth(AutoStructuredSpewer& spew,
                                      HandleScript script, ICEntry* entry,
                                      ICFallbackStub* fallback, jsbytecode* pc,
                                      JSOp op, Happiness* entryHappiness) {
  spew->property("op", CodeName(op));

  // TODO: If a perf issue arises, look into improving the SrcNotes
  // API call below.
  unsigned column;
  spew->property("lineno", PCToLineNumber(script, pc, &column));
  spew->property("column", column);

  ICStub* firstStub = entry->firstStub();
  if (!firstStub->isFallback()) {
    if (!spewNonFallbackICInformation(spew, firstStub, entryHappiness)) {
      return false;
    }
  }

  if (fallback->state().mode() != ICState::Mode::Specialized) {
    *entryHappiness = Sad;
  }

  spew->property("entryHappiness", uint8_t(*entryHappiness));

  spew->property("mode", uint8_t(fallback->state().mode()));

  spew->property("fallbackCount", fallback->enteredCount());

  return true;
}

void CacheIRHealth::spewScriptFinalWarmUpCount(JSContext* cx,
                                               const char* filename,
                                               JSScript* script,
                                               uint32_t warmUpCount) {
  AutoStructuredSpewer spew(cx, SpewChannel::CacheIRHealthReport, nullptr);
  if (!spew) {
    return;
  }

  spew->property("filename", filename);
  spew->property("line", script->lineno());
  spew->property("column", script->column());
  spew->property("finalWarmUpCount", warmUpCount);
}

static bool addScriptToFinalWarmUpCountMap(JSContext* cx, HandleScript script) {
  // Create Zone::scriptFilenameMap if necessary.
  JS::Zone* zone = script->zone();
  if (!zone->scriptFinalWarmUpCountMap) {
    auto map = MakeUnique<ScriptFinalWarmUpCountMap>();
    if (!map) {
      return false;
    }

    zone->scriptFinalWarmUpCountMap = std::move(map);
  }

  auto* filename = js_pod_malloc<char>(strlen(script->filename()) + 1);
  if (!filename) {
    return false;
  }
  strcpy(filename, script->filename());

  if (!zone->scriptFinalWarmUpCountMap->put(
          script, mozilla::MakeTuple(uint32_t(0), filename))) {
    js_free(filename);
    return false;
  }

  script->setNeedsFinalWarmUpCount();
  return true;
}

void CacheIRHealth::healthReportForIC(JSContext* cx, ICEntry* entry,
                                      ICFallbackStub* fallback,
                                      HandleScript script,
                                      SpewContext context) {
  AutoStructuredSpewer spew(cx, SpewChannel::CacheIRHealthReport, script);
  if (!spew) {
    return;
  }

  if (!addScriptToFinalWarmUpCountMap(cx, script)) {
    cx->recoverFromOutOfMemory();
    return;
  }
  spew->property("spewContext", uint8_t(context));

  jsbytecode* op = fallback->pc(script);
  JSOp jsOp = JSOp(*op);

  Happiness entryHappiness = Happy;
  if (!spewICEntryHealth(spew, script, entry, fallback, op, jsOp,
                         &entryHappiness)) {
    cx->recoverFromOutOfMemory();
    return;
  }
  MOZ_ASSERT(entryHappiness == Sad);
}

void CacheIRHealth::healthReportForScript(JSContext* cx, HandleScript script,
                                          SpewContext context) {
  jit::JitScript* jitScript = script->maybeJitScript();
  if (!jitScript) {
    return;
  }

  AutoStructuredSpewer spew(cx, SpewChannel::CacheIRHealthReport, script);
  if (!spew) {
    return;
  }

  if (!addScriptToFinalWarmUpCountMap(cx, script)) {
    cx->recoverFromOutOfMemory();
    return;
  }

  spew->property("spewContext", uint8_t(context));

  spew->beginListProperty("entries");

  Happiness scriptHappiness = Happy;

  for (size_t i = 0; i < jitScript->numICEntries(); i++) {
    ICEntry& entry = jitScript->icEntry(i);
    ICFallbackStub* fallback = jitScript->fallbackStub(i);
    jsbytecode* pc = fallback->pc(script);
    JSOp op = JSOp(*pc);

    spew->beginObject();
    Happiness entryHappiness = Happy;
    if (!spewICEntryHealth(spew, script, &entry, fallback, pc, op,
                           &entryHappiness)) {
      cx->recoverFromOutOfMemory();
      return;
    }
    if (entryHappiness < scriptHappiness) {
      scriptHappiness = entryHappiness;
    }
    spew->endObject();
  }

  spew->endList();  // entries

  spew->property("scriptHappiness", uint8_t(scriptHappiness));
}

#endif /* JS_CACHEIR_SPEW */
