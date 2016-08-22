/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/collation/collator_interface_icu.h"

#include <unicode/coll.h>
#include <unicode/sortkey.h>

#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"

namespace mongo {

CollatorInterfaceICU::CollatorInterfaceICU(CollationSpec spec,
                                           std::unique_ptr<icu::Collator> collator)
    : CollatorInterface(std::move(spec)), _collator(std::move(collator)) {}

std::unique_ptr<CollatorInterface> CollatorInterfaceICU::clone() const {
    auto clone = stdx::make_unique<CollatorInterfaceICU>(
        getSpec(), std::unique_ptr<icu::Collator>(_collator->clone()));
    return {std::move(clone)};
}

int CollatorInterfaceICU::compare(StringData left, StringData right) const {
    UErrorCode status = U_ZERO_ERROR;
    auto compareResult = _collator->compareUTF8(icu::StringPiece(left.rawData(), left.size()),
                                                icu::StringPiece(right.rawData(), right.size()),
                                                status);

    // Any sequence of bytes, even invalid UTF-8, has defined comparison behavior in ICU (invalid
    // subsequences are weighted as the replacement character, U+FFFD). A non-ok error code is only
    // expected when a memory allocation fails inside ICU, which we consider fatal to the process.
    fassert(34438, U_SUCCESS(status));

    switch (compareResult) {
        case UCOL_EQUAL:
            return 0;
        case UCOL_GREATER:
            return 1;
        case UCOL_LESS:
            return -1;
    }

    MONGO_UNREACHABLE;
}

CollatorInterface::ComparisonKey CollatorInterfaceICU::getComparisonKey(
    StringData stringData) const {
    // A StringPiece is ICU's StringData. They are logically the same abstraction.
    const icu::StringPiece stringPiece(stringData.rawData(), stringData.size());

    UErrorCode status = U_ZERO_ERROR;
    icu::CollationKey icuKey;
    _collator->getCollationKey(icu::UnicodeString::fromUTF8(stringPiece), icuKey, status);

    // Any sequence of bytes, even invalid UTF-8, has defined comparison behavior in ICU (invalid
    // subsequences are weighted as the replacement character, U+FFFD). A non-ok error code is only
    // expected when a memory allocation fails inside ICU, which we consider fatal to the process.
    fassert(34439, U_SUCCESS(status));

    int32_t keyLength;
    const uint8_t* keyBuffer = icuKey.getByteArray(keyLength);
    invariant(keyLength > 0);
    invariant(keyBuffer);

    // The last byte of the sort key should always be null. When we construct the comparison key, we
    // omit the trailing null byte.
    invariant(keyBuffer[keyLength - 1u] == '\0');
    const char* charBuffer = reinterpret_cast<const char*>(keyBuffer);
    return makeComparisonKey(std::string(charBuffer, keyLength - 1u));
}

}  // namespace mongo
