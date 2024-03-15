/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/StaticStrings.h"

#include "mozilla/HashFunctions.h"  // mozilla::HashString

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "js/HashTable.h"   // js::HashNumber
#include "js/TypeDecls.h"   // Latin1Char
#include "vm/Realm.h"       // AutoAllocInAtomsZone
#include "vm/StringType.h"  // JSString, JSLinearString

#include "vm/Realm-inl.h"       // AutoAllocInAtomsZone
#include "vm/StringType-inl.h"  // NewInlineAtom

using namespace js;

constexpr StaticStrings::SmallCharTable StaticStrings::createSmallCharTable() {
  SmallCharTable array{};
  for (size_t i = 0; i < SMALL_CHAR_TABLE_SIZE; i++) {
    array[i] = toSmallChar(i);
  }
  return array;
}

const StaticStrings::SmallCharTable StaticStrings::toSmallCharTable =
    createSmallCharTable();

bool StaticStrings::init(JSContext* cx) {
  AutoAllocInAtomsZone az(cx);

  static_assert(UNIT_STATIC_LIMIT - 1 <= JSString::MAX_LATIN1_CHAR,
                "Unit strings must fit in Latin1Char.");

  for (uint32_t i = 0; i < UNIT_STATIC_LIMIT; i++) {
    Latin1Char ch = Latin1Char(i);
    HashNumber hash = mozilla::HashString(&ch, 1);
    JSAtom* a = NewInlineAtom(cx, &ch, 1, hash);
    if (!a) {
      return false;
    }
    a->makePermanent();
    unitStaticTable[i] = a;
  }

  for (uint32_t i = 0; i < NUM_LENGTH2_ENTRIES; i++) {
    Latin1Char buffer[] = {firstCharOfLength2(i), secondCharOfLength2(i)};
    HashNumber hash = mozilla::HashString(buffer, 2);
    JSAtom* a = NewInlineAtom(cx, buffer, 2, hash);
    if (!a) {
      return false;
    }
    a->makePermanent();
    length2StaticTable[i] = a;
  }

  for (uint32_t i = 0; i < INT_STATIC_LIMIT; i++) {
    if (i < 10) {
      intStaticTable[i] = unitStaticTable[i + '0'];
    } else if (i < 100) {
      auto index =
          getLength2IndexStatic(char(i / 10) + '0', char(i % 10) + '0');
      intStaticTable[i] = length2StaticTable[index];
    } else {
      Latin1Char buffer[] = {Latin1Char(firstCharOfLength3(i)),
                             Latin1Char(secondCharOfLength3(i)),
                             Latin1Char(thirdCharOfLength3(i))};
      HashNumber hash = mozilla::HashString(buffer, 3);
      JSAtom* a = NewInlineAtom(cx, buffer, 3, hash);
      if (!a) {
        return false;
      }
      a->makePermanent();
      intStaticTable[i] = a;
    }

    // Static string initialization can not race, so allow even without the
    // lock.
    intStaticTable[i]->setIsIndex(i);
  }

  return true;
}
