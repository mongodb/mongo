/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <stdint.h>

namespace mozilla {
namespace unicode {

// Multi-character mappings (from SpecialCasing.txt) map a single Unicode
// value to a sequence of 2 or 3 Unicode characters. There are currently none
// defined outside the BMP, so we can use char16_t here. Unused trailing
// positions in mMappedChars are set to 0.
struct MultiCharMapping {
  char16_t mOriginalChar;
  char16_t mMappedChars[3];
};

// Return a pointer to the special case mapping for the given character;
// returns nullptr if no such mapping is defined.
const MultiCharMapping* SpecialUpper(uint32_t aCh);
const MultiCharMapping* SpecialLower(uint32_t aCh);
const MultiCharMapping* SpecialTitle(uint32_t aCh);

}  // namespace unicode
}  // namespace mozilla
