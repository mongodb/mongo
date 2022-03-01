/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef intl_components_NumberFormatFields_h_
#define intl_components_NumberFormatFields_h_
#include "mozilla/intl/NumberFormat.h"
#include "mozilla/Vector.h"

namespace mozilla {
namespace intl {

struct NumberFormatField {
  uint32_t begin;
  uint32_t end;
  NumberPartType type;

  // Needed for vector-resizing scratch space.
  NumberFormatField() = default;

  NumberFormatField(uint32_t begin, uint32_t end, NumberPartType type)
      : begin(begin), end(end), type(type) {}
};

class NumberFormatFields {
  using FieldsVector = Vector<NumberFormatField, 16>;

  FieldsVector fields_;

 public:
  [[nodiscard]] bool append(NumberPartType type, int32_t begin, int32_t end);

  [[nodiscard]] bool toPartsVector(size_t overallLength,
                                   NumberPartVector& parts);
};

}  // namespace intl
}  // namespace mozilla

#endif
