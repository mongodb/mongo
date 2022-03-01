/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmBinary.h"

#include "js/Printf.h"
#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::wasm;

// Decoder implementation.

bool Decoder::failf(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  UniqueChars str(JS_vsmprintf(msg, ap));
  va_end(ap);
  if (!str) {
    return false;
  }

  return fail(str.get());
}

void Decoder::warnf(const char* msg, ...) {
  if (!warnings_) {
    return;
  }

  va_list ap;
  va_start(ap, msg);
  UniqueChars str(JS_vsmprintf(msg, ap));
  va_end(ap);
  if (!str) {
    return;
  }

  (void)warnings_->append(std::move(str));
}

bool Decoder::fail(size_t errorOffset, const char* msg) {
  MOZ_ASSERT(error_);
  UniqueChars strWithOffset(JS_smprintf("at offset %zu: %s", errorOffset, msg));
  if (!strWithOffset) {
    return false;
  }

  *error_ = std::move(strWithOffset);
  return false;
}

bool Decoder::readSectionHeader(uint8_t* id, SectionRange* range) {
  if (!readFixedU8(id)) {
    return false;
  }

  uint32_t size;
  if (!readVarU32(&size)) {
    return false;
  }

  range->start = currentOffset();
  range->size = size;
  return true;
}

bool Decoder::startSection(SectionId id, ModuleEnvironment* env,
                           MaybeSectionRange* range, const char* sectionName) {
  MOZ_ASSERT(!*range);

  // Record state at beginning of section to allow rewinding to this point
  // if, after skipping through several custom sections, we don't find the
  // section 'id'.
  const uint8_t* const initialCur = cur_;
  const size_t initialCustomSectionsLength = env->customSections.length();

  // Maintain a pointer to the current section that gets updated as custom
  // sections are skipped.
  const uint8_t* currentSectionStart = cur_;

  // Only start a section with 'id', skipping any custom sections before it.

  uint8_t idValue;
  if (!readFixedU8(&idValue)) {
    goto rewind;
  }

  while (idValue != uint8_t(id)) {
    if (idValue != uint8_t(SectionId::Custom)) {
      goto rewind;
    }

    // Rewind to the beginning of the current section since this is what
    // skipCustomSection() assumes.
    cur_ = currentSectionStart;
    if (!skipCustomSection(env)) {
      return false;
    }

    // Having successfully skipped a custom section, consider the next
    // section.
    currentSectionStart = cur_;
    if (!readFixedU8(&idValue)) {
      goto rewind;
    }
  }

  // Don't check the size since the range of bytes being decoded might not
  // contain the section body. (This is currently the case when streaming: the
  // code section header is decoded with the module environment bytes, the
  // body of the code section is streamed in separately.)

  uint32_t size;
  if (!readVarU32(&size)) {
    goto fail;
  }

  range->emplace();
  (*range)->start = currentOffset();
  (*range)->size = size;
  return true;

rewind:
  cur_ = initialCur;
  env->customSections.shrinkTo(initialCustomSectionsLength);
  return true;

fail:
  return failf("failed to start %s section", sectionName);
}

bool Decoder::finishSection(const SectionRange& range,
                            const char* sectionName) {
  if (resilientMode_) {
    return true;
  }
  if (range.size != currentOffset() - range.start) {
    return failf("byte size mismatch in %s section", sectionName);
  }
  return true;
}

bool Decoder::startCustomSection(const char* expected, size_t expectedLength,
                                 ModuleEnvironment* env,
                                 MaybeSectionRange* range) {
  // Record state at beginning of section to allow rewinding to this point
  // if, after skipping through several custom sections, we don't find the
  // section 'id'.
  const uint8_t* const initialCur = cur_;
  const size_t initialCustomSectionsLength = env->customSections.length();

  while (true) {
    // Try to start a custom section. If we can't, rewind to the beginning
    // since we may have skipped several custom sections already looking for
    // 'expected'.
    if (!startSection(SectionId::Custom, env, range, "custom")) {
      return false;
    }
    if (!*range) {
      goto rewind;
    }

    if (bytesRemain() < (*range)->size) {
      goto fail;
    }

    CustomSectionEnv sec;
    if (!readVarU32(&sec.nameLength) || sec.nameLength > bytesRemain()) {
      goto fail;
    }

    sec.nameOffset = currentOffset();
    sec.payloadOffset = sec.nameOffset + sec.nameLength;

    uint32_t payloadEnd = (*range)->start + (*range)->size;
    if (sec.payloadOffset > payloadEnd) {
      goto fail;
    }

    sec.payloadLength = payloadEnd - sec.payloadOffset;

    // Now that we have a valid custom section, record its offsets in the
    // metadata which can be queried by the user via Module.customSections.
    // Note: after an entry is appended, it may be popped if this loop or
    // the loop in startSection needs to rewind.
    if (!env->customSections.append(sec)) {
      return false;
    }

    // If this is the expected custom section, we're done.
    if (!expected || (expectedLength == sec.nameLength &&
                      !memcmp(cur_, expected, sec.nameLength))) {
      cur_ += sec.nameLength;
      return true;
    }

    // Otherwise, blindly skip the custom section and keep looking.
    skipAndFinishCustomSection(**range);
    range->reset();
  }
  MOZ_CRASH("unreachable");

rewind:
  cur_ = initialCur;
  env->customSections.shrinkTo(initialCustomSectionsLength);
  return true;

fail:
  return fail("failed to start custom section");
}

void Decoder::finishCustomSection(const char* name, const SectionRange& range) {
  MOZ_ASSERT(cur_ >= beg_);
  MOZ_ASSERT(cur_ <= end_);

  if (error_ && *error_) {
    warnf("in the '%s' custom section: %s", name, error_->get());
    skipAndFinishCustomSection(range);
    return;
  }

  uint32_t actualSize = currentOffset() - range.start;
  if (range.size != actualSize) {
    if (actualSize < range.size) {
      warnf("in the '%s' custom section: %" PRIu32 " unconsumed bytes", name,
            uint32_t(range.size - actualSize));
    } else {
      warnf("in the '%s' custom section: %" PRIu32
            " bytes consumed past the end",
            name, uint32_t(actualSize - range.size));
    }
    skipAndFinishCustomSection(range);
    return;
  }

  // Nothing to do! (c.f. skipAndFinishCustomSection())
}

void Decoder::skipAndFinishCustomSection(const SectionRange& range) {
  MOZ_ASSERT(cur_ >= beg_);
  MOZ_ASSERT(cur_ <= end_);
  cur_ = (beg_ + (range.start - offsetInModule_)) + range.size;
  MOZ_ASSERT(cur_ <= end_);
  clearError();
}

bool Decoder::skipCustomSection(ModuleEnvironment* env) {
  MaybeSectionRange range;
  if (!startCustomSection(nullptr, 0, env, &range)) {
    return false;
  }
  if (!range) {
    return fail("expected custom section");
  }

  skipAndFinishCustomSection(*range);
  return true;
}

bool Decoder::startNameSubsection(NameType nameType,
                                  Maybe<uint32_t>* endOffset) {
  MOZ_ASSERT(!*endOffset);

  const uint8_t* const initialPosition = cur_;

  uint8_t nameTypeValue;
  if (!readFixedU8(&nameTypeValue)) {
    goto rewind;
  }

  if (nameTypeValue != uint8_t(nameType)) {
    goto rewind;
  }

  uint32_t payloadLength;
  if (!readVarU32(&payloadLength) || payloadLength > bytesRemain()) {
    return fail("bad name subsection payload length");
  }

  *endOffset = Some(currentOffset() + payloadLength);
  return true;

rewind:
  cur_ = initialPosition;
  return true;
}

bool Decoder::finishNameSubsection(uint32_t endOffset) {
  uint32_t actual = currentOffset();
  if (endOffset != actual) {
    return failf("bad name subsection length (endOffset: %" PRIu32
                 ", actual: %" PRIu32 ")",
                 endOffset, actual);
  }

  return true;
}

bool Decoder::skipNameSubsection() {
  uint8_t nameTypeValue;
  if (!readFixedU8(&nameTypeValue)) {
    return fail("unable to read name subsection id");
  }

  switch (nameTypeValue) {
    case uint8_t(NameType::Module):
    case uint8_t(NameType::Function):
      return fail("out of order name subsections");
    default:
      break;
  }

  uint32_t payloadLength;
  if (!readVarU32(&payloadLength) || !readBytes(payloadLength)) {
    return fail("bad name subsection payload length");
  }

  return true;
}
