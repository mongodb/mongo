// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/modules.h"

#include <span>
#include <string_view>

namespace mongo::sbe::value {
class KeyStringEntry {
public:
    KeyStringEntry() = default;

    explicit KeyStringEntry(const SortedDataKeyValueView& view);

    explicit KeyStringEntry(key_string::Value value);

    KeyStringEntry& operator=(KeyStringEntry&& other) noexcept;

    bool owned() const {
        return _value != boost::none;
    }

    std::span<const char> getKeyStringView() const {
        return _key;
    }

    std::span<const char> getTypeBitsView() const {
        return _typeBits;
    }

    std::span<const char> getRecordIdView() const {
        return _rid;
    }

    key_string::Version getVersion() const {
        return _version;
    }

    int getSerializedSize() const {
        return sizeof(_version) + _key.size() + _typeBits.size();
    }

    int compare(const KeyStringEntry& other) const {
        return key_string::compare(_key, other._key);
    }

    std::size_t hash() const {
        return absl::Hash<absl::string_view>{}(absl::string_view{_key.data(), _key.size()});
    }

    const key_string::Value& getValue() const {
        tassert(8843701, "value is not available", _value);
        return *_value;
    }

    key_string::Value getValueCopy() const {
        return key_string::Value::makeValue(_version, _key, _rid, _typeBits);
    }

    std::unique_ptr<KeyStringEntry> makeCopy() const;

    // Serializes key string (without record id) into a storable format with TypeBits information.
    // The serialized format takes the following form:
    //   [version][keystring size][keystring encoding][typebits encoding]
    void serialize(BufBuilder& buf) const;

    // Deserialize the Value from a serialized format.
    static KeyStringEntry* deserialize(BufReader& buf);

    std::string toString() const {
        return hexblob::encode({_key.data(), _key.size()});
    }

private:
    std::span<const char> _key;
    std::span<const char> _typeBits;
    // For indexConsistencyCheck.
    std::span<const char> _rid;
    key_string::Version _version;
    boost::optional<key_string::Value> _value;  // none means unowned view.

    void _initFromView(const SortedDataKeyValueView& view);
};
}  // namespace mongo::sbe::value
