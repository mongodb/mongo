// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/compact_hash_agg_state.h"

#include "mongo/util/scopeguard.h"

#include <new>


namespace mongo::sbe {

/**
 * Destroys every accumulator state and frees the buffer while the buffer is not yet in a
 * CompactStateTable.
 */
struct CompactStateLayout::Deleter {
    void operator()(std::byte* buffer) const {
        layout->destroy(buffer);
    }

    const CompactStateLayout* layout;
};

CompactStateLayout::OwnedStates CompactStateLayout::create() const {
    // Don't allocate a buffer when there are no accumulators.
    if (_size == 0) {
        return OwnedStates{nullptr, Deleter{this}};
    }
    auto buffer = static_cast<std::byte*>(::operator new(_size, std::align_val_t{_alignment}));
    // Constructing the byte buffer calls constructors for each accumulator state in the $group. If
    // one of those constructors throws an exception, we need to clean up the already-constructed
    // states before it. 'built' tracks how many states have been constructed so far.
    size_t built = 0;
    ScopeGuard destroyBuilt([&] { destroy(buffer, built); });
    for (const Entry& entry : _entries) {
        entry.construct(buffer);
        ++built;
    }
    destroyBuilt.dismiss();
    return OwnedStates{buffer, Deleter{this}};
}

void CompactStateLayout::destroy(std::byte* buffer, size_t built) const {
    if (!_triviallyDestructible) {
        for (size_t i = built; i-- > 0;) {
            _entries[i].destroy(buffer);
        }
    }
    ::operator delete(buffer, _size, std::align_val_t{_alignment});
}

CompactStateTable::iterator CompactStateTable::findOrInsert(const value::MaterializedRow& key) {
    if (iterator it = _map.find(key); it != _map.end()) {
        return it;
    }
    value::MaterializedRow keyCopy(key);
    keyCopy.makeOwned();
    CompactStateLayout::OwnedStates states = _layout.create();
    iterator it = _map.emplace(std::move(keyCopy), states.get()).first;
    (void)states.release();
    return it;
}

void CompactStateTable::clear() {
    for (auto&& [_, buffer] : _map) {
        _layout.destroy(buffer);
    }
    _map.clear();
}

}  // namespace mongo::sbe
