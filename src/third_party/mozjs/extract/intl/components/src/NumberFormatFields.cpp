/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "NumberFormatFields.h"

namespace mozilla {
namespace intl {

bool NumberFormatFields::append(NumberPartType type, int32_t begin,
                                int32_t end) {
  MOZ_ASSERT(begin >= 0);
  MOZ_ASSERT(end >= 0);
  MOZ_ASSERT(begin < end, "erm, aren't fields always non-empty?");

  return fields_.emplaceBack(uint32_t(begin), uint32_t(end), type);
}

bool NumberFormatFields::toPartsVector(size_t overallLength,
                                       NumberPartVector& parts) {
  std::sort(fields_.begin(), fields_.end(),
            [](const NumberFormatField& left, const NumberFormatField& right) {
              // Sort first by begin index, then to place
              // enclosing fields before nested fields.
              return left.begin < right.begin ||
                     (left.begin == right.begin && left.end > right.end);
            });

  // Then iterate over the sorted field list to generate a sequence of parts
  // (what ECMA-402 actually exposes).  A part is a maximal character sequence
  // entirely within no field or a single most-nested field.
  //
  // Diagrams may be helpful to illustrate how fields map to parts.  Consider
  // formatting -19,766,580,028,249.41, the US national surplus (negative
  // because it's actually a debt) on October 18, 2016.
  //
  //    var options =
  //      { style: "currency", currency: "USD", currencyDisplay: "name" };
  //    var usdFormatter = new Intl.NumberFormat("en-US", options);
  //    usdFormatter.format(-19766580028249.41);
  //
  // The formatted result is "-19,766,580,028,249.41 US dollars".  ICU
  // identifies these fields in the string:
  //
  //     UNUM_GROUPING_SEPARATOR_FIELD
  //                   |
  //   UNUM_SIGN_FIELD |  UNUM_DECIMAL_SEPARATOR_FIELD
  //    |   __________/|   |
  //    |  /   |   |   |   |
  //   "-19,766,580,028,249.41 US dollars"
  //     \________________/ |/ \_______/
  //             |          |      |
  //    UNUM_INTEGER_FIELD  |  UNUM_CURRENCY_FIELD
  //                        |
  //               UNUM_FRACTION_FIELD
  //
  // These fields map to parts as follows:
  //
  //         integer     decimal
  //       _____|________  |
  //      /  /| |\  |\  |\ |  literal
  //     /| / | | \ | \ | \|  |
  //   "-19,766,580,028,249.41 US dollars"
  //    |  \___|___|___/    |/ \________/
  //    |        |          |       |
  //    |      group        |   currency
  //    |                   |
  //   minusSign        fraction
  //
  // The sign is a part.  Each comma is a part, splitting the integer field
  // into parts for trillions/billions/&c. digits.  The decimal point is a
  // part.  Cents are a part.  The space between cents and currency is a part
  // (outside any field).  Last, the currency field is a part.

  class PartGenerator {
    // The fields in order from start to end, then least to most nested.
    const FieldsVector& fields;

    // Index of the current field, in |fields|, being considered to
    // determine part boundaries.  |lastEnd <= fields[index].begin| is an
    // invariant.
    size_t index = 0;

    // The end index of the last part produced, always less than or equal
    // to |limit|, strictly increasing.
    uint32_t lastEnd = 0;

    // The length of the overall formatted string.
    const uint32_t limit = 0;

    Vector<size_t, 4> enclosingFields;

    void popEnclosingFieldsEndingAt(uint32_t end) {
      MOZ_ASSERT_IF(enclosingFields.length() > 0,
                    fields[enclosingFields.back()].end >= end);

      while (enclosingFields.length() > 0 &&
             fields[enclosingFields.back()].end == end) {
        enclosingFields.popBack();
      }
    }

    bool nextPartInternal(NumberPart* part) {
      size_t len = fields.length();
      MOZ_ASSERT(index <= len);

      // If we're out of fields, all that remains are part(s) consisting
      // of trailing portions of enclosing fields, and maybe a final
      // literal part.
      if (index == len) {
        if (enclosingFields.length() > 0) {
          const auto& enclosing = fields[enclosingFields.popCopy()];
          part->first = enclosing.type;
          part->second = enclosing.end;

          // If additional enclosing fields end where this part ends,
          // pop them as well.
          popEnclosingFieldsEndingAt(part->second);
        } else {
          part->first = NumberPartType::Literal;
          part->second = limit;
        }

        return true;
      }

      // Otherwise we still have a field to process.
      const NumberFormatField* current = &fields[index];
      MOZ_ASSERT(lastEnd <= current->begin);
      MOZ_ASSERT(current->begin < current->end);

      // But first, deal with inter-field space.
      if (lastEnd < current->begin) {
        if (enclosingFields.length() > 0) {
          // Space between fields, within an enclosing field, is part
          // of that enclosing field, until the start of the current
          // field or the end of the enclosing field, whichever is
          // earlier.
          const auto& enclosing = fields[enclosingFields.back()];
          part->first = enclosing.type;
          part->second = std::min(enclosing.end, current->begin);
          popEnclosingFieldsEndingAt(part->second);
        } else {
          // If there's no enclosing field, the space is a literal.
          part->first = NumberPartType::Literal;
          part->second = current->begin;
        }

        return true;
      }

      // Otherwise, the part spans a prefix of the current field.  Find
      // the most-nested field containing that prefix.
      const NumberFormatField* next;
      do {
        current = &fields[index];

        // If the current field is last, the part extends to its end.
        if (++index == len) {
          part->first = current->type;
          part->second = current->end;
          return true;
        }

        next = &fields[index];
        MOZ_ASSERT(current->begin <= next->begin);
        MOZ_ASSERT(current->begin < next->end);

        // If the next field nests within the current field, push an
        // enclosing field.  (If there are no nested fields, don't
        // bother pushing a field that'd be immediately popped.)
        if (current->end > next->begin) {
          if (!enclosingFields.append(index - 1)) {
            return false;
          }
        }

        // Do so until the next field begins after this one.
      } while (current->begin == next->begin);

      part->first = current->type;

      if (current->end <= next->begin) {
        // The next field begins after the current field ends.  Therefore
        // the current part ends at the end of the current field.
        part->second = current->end;
        popEnclosingFieldsEndingAt(part->second);
      } else {
        // The current field encloses the next one.  The current part
        // ends where the next field/part will start.
        part->second = next->begin;
      }

      return true;
    }

   public:
    PartGenerator(const FieldsVector& vec, uint32_t limit)
        : fields(vec), limit(limit), enclosingFields() {}

    bool nextPart(bool* hasPart, NumberPart* part) {
      // There are no parts left if we've partitioned the entire string.
      if (lastEnd == limit) {
        MOZ_ASSERT(enclosingFields.length() == 0);
        *hasPart = false;
        return true;
      }

      if (!nextPartInternal(part)) {
        return false;
      }

      *hasPart = true;
      lastEnd = part->second;
      return true;
    }
  };

  // Finally, generate the result array.
  size_t lastEndIndex = 0;

  PartGenerator gen(fields_, overallLength);
  do {
    bool hasPart;
    NumberPart part;
    if (!gen.nextPart(&hasPart, &part)) {
      return false;
    }

    if (!hasPart) {
      break;
    }

    MOZ_ASSERT(lastEndIndex < part.second);

    if (!parts.append(part)) {
      return false;
    }

    lastEndIndex = part.second;
  } while (true);

  MOZ_ASSERT(lastEndIndex == overallLength,
             "result array must partition the entire string");

  return lastEndIndex == overallLength;
}

}  // namespace intl
}  // namespace mozilla
