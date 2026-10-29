// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/ordering.h"
#include "mongo/db/exec/sbe/vm/vm.h"

#include <cstddef>
#include <cstdint>

#include <absl/container/inlined_vector.h>

namespace mongo {
namespace sbe {
namespace vm {
value::TagValueMaybeOwned ByteCode::genericNewKeyString(ArityType arity,
                                                        CollatorInterface* collator) {
    auto versionView = viewFromStack(0);
    auto orderingView = viewFromStack(1);
    auto discriminatorView = viewFromStack(arity - 1u);
    if (!value::isNumber(versionView.tag) || !value::isNumber(orderingView.tag) ||
        !value::isNumber(discriminatorView.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto version = value::numericCast<int64_t>(versionView);
    auto discriminator = value::numericCast<int64_t>(discriminatorView);
    if ((version < 0 || version > 1) || (discriminator < 0 || discriminator > 2)) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto ksVersion = static_cast<key_string::Version>(version);
    auto ksDiscriminator = static_cast<key_string::Discriminator>(discriminator);

    uint32_t orderingBits = value::numericCast<int32_t>(orderingView);

    // Maximum number of orderings. An 'Ordering' cannot have more than 32 values at the moment.
    // Limit the usage to 32 bytes here anyway, because if that definition ever changes, the amount
    // of stack space used for the inline vector will still be reasonably bounded.
    constexpr size_t kMaxInlineValues = std::min(size_t(32), Ordering::kMaxCompoundIndexKeys);
    absl::InlinedVector<int8_t, kMaxInlineValues> orders;
    for (size_t i = 0; orderingBits != 0 && i < arity - 3u; ++i, orderingBits >>= 1) {
        orders.push_back((orderingBits & 1) ? -1 : 1);
    }

    key_string::HeapBuilder kb{ksVersion, Ordering::make(orders)};

    for (size_t idx = 2; idx < arity - 1u; ++idx) {
        sbe::value::appendToKeyString(kb, viewFromStack(idx), collator);
    }

    kb.appendDiscriminator(ksDiscriminator);

    return value::TagValueMaybeOwned::fromRaw(
        true, value::TypeTags::keyString, value::makeKeyString(kb.release()).second);
}  // genericNewKeyString

value::TagValueMaybeOwned ByteCode::builtinNewKeyString(ArityType arity) {
    tassert(6333000,
            str::stream() << "Unsupported number of arguments passed to ks(): " << arity,
            arity >= 3 && arity <= Ordering::kMaxCompoundIndexKeys + 3);
    return genericNewKeyString(arity);
}

value::TagValueMaybeOwned ByteCode::builtinCollNewKeyString(ArityType arity) {
    tassert(6511500,
            str::stream() << "Unsupported number of arguments passed to collKs(): " << arity,
            arity >= 4 && arity <= Ordering::kMaxCompoundIndexKeys + 4);

    auto collatorView = viewFromStack(arity - 1u);
    if (collatorView.tag != value::TypeTags::collator) {
        return value::TagValueMaybeOwned::nothing();
    }
    auto collator = value::getCollatorView(collatorView.value);
    return genericNewKeyString(arity - 1u, collator);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
