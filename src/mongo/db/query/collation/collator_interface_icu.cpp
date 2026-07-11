// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/collation/collator_interface_icu.h"

#include "mongo/util/assert_util.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <unicode/coll.h>
#include <unicode/sortkey.h>
#include <unicode/stringpiece.h>
#include <unicode/ucol.h>
#include <unicode/unistr.h>
#include <unicode/utypes.h>

namespace mongo {

CollatorInterfaceICU::CollatorInterfaceICU(Collation spec, std::unique_ptr<icu::Collator> collator)
    : CollatorInterface(std::move(spec)), _collator(std::move(collator)) {}

std::unique_ptr<CollatorInterface> CollatorInterfaceICU::clone() const {
    return std::make_unique<CollatorInterfaceICU>(
        getSpec(), std::unique_ptr<icu::Collator>(_collator->clone()));
}

std::shared_ptr<CollatorInterface> CollatorInterfaceICU::cloneShared() const {
    return std::make_shared<CollatorInterfaceICU>(
        getSpec(), std::unique_ptr<icu::Collator>(_collator->clone()));
}

int CollatorInterfaceICU::compare(std::string_view left, std::string_view right) const {
    UErrorCode status = U_ZERO_ERROR;
    auto compareResult = _collator->compareUTF8(icu::StringPiece(left.data(), left.size()),
                                                icu::StringPiece(right.data(), right.size()),
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
    std::string_view stringData) const {
    // A StringPiece is ICU's std::string_view. They are logically the same abstraction.
    const icu::StringPiece stringPiece(stringData.data(), stringData.size());

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
