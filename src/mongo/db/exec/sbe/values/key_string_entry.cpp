// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/key_string_entry.h"

#include <string_view>

namespace mongo::sbe::value {

KeyStringEntry::KeyStringEntry(const SortedDataKeyValueView& view) {
    _initFromView(view);
}

KeyStringEntry::KeyStringEntry(key_string::Value value) : _value(std::move(value)) {
    auto view = SortedDataKeyValueView::fromValue(*_value);
    _initFromView(view);
}

KeyStringEntry& KeyStringEntry::operator=(KeyStringEntry&& other) noexcept {
    _key = other._key;
    _rid = other._rid;
    _typeBits = other._typeBits;
    _version = other._version;
    _value = std::move(other._value);
    return *this;
}

std::unique_ptr<KeyStringEntry> KeyStringEntry::makeCopy() const {
    return std::make_unique<KeyStringEntry>(
        key_string::Value::makeValue(_version, _key, _rid, _typeBits));
}

void KeyStringEntry::serialize(BufBuilder& buf) const {
    buf.appendUChar(static_cast<uint8_t>(_version));
    buf.appendNum(_key.size());
    buf.appendBuf(_key.data(), _key.size());
    buf.appendNum(_typeBits.size());
    buf.appendBuf(_typeBits.data(), _typeBits.size());
    buf.appendNum(_rid.size());
    buf.appendBuf(_rid.data(), _rid.size());
}

KeyStringEntry* KeyStringEntry::deserialize(BufReader& buf) {
    auto version = static_cast<key_string::Version>(buf.read<uint8_t>());
    auto key = buf.readBytes(buf.read<LittleEndian<std::string_view::size_type>>());
    auto typeBits = buf.readBytes(buf.read<LittleEndian<std::string_view::size_type>>());
    auto rid = buf.readBytes(buf.read<LittleEndian<std::string_view::size_type>>());
    return new KeyStringEntry{key_string::Value::makeValue(version, key, rid, typeBits)};
}

void KeyStringEntry::_initFromView(const SortedDataKeyValueView& view) {
    _key = view.getKeyStringWithoutRecordIdView();
    _typeBits = view.getTypeBitsView();
    _rid = view.getRecordIdView();
    _version = view.getVersion();
}
}  // namespace mongo::sbe::value
