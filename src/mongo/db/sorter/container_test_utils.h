// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/recovery_unit.h"

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace mongo::sorter {

class ViewableIntegerKeyedContainer final : public IntegerKeyedContainer {
public:
    using Entry = std::pair<int64_t, std::string>;

    class Cursor : public IntegerKeyedContainer::Cursor {
    public:
        explicit Cursor(const std::vector<ViewableIntegerKeyedContainer::Entry>& entries)
            : _entries(entries) {}

        boost::optional<std::span<const char>> find(int64_t key) override {
            for (auto&& [entryKey, entryValue] : _entries) {
                if (entryKey == key) {
                    _position = entryKey;
                    return {{entryValue.data(), entryValue.size()}};
                }
            }
            return boost::none;
        }

        boost::optional<std::pair<int64_t, std::span<const char>>> next() override {
            if (!_position) {
                _position = _entries.begin()->first;
                return {{_entries.begin()->first,
                         {_entries.begin()->second.data(), _entries.begin()->second.size()}}};
            }

            for (auto it = _entries.begin(); it != _entries.end(); ++it) {
                if (it->first == _position) {
                    ++it;
                    if (it == _entries.end()) {
                        return boost::none;
                    }
                    _position = it->first;
                    return {{it->first, {it->second.data(), it->second.size()}}};
                }
            }
            return boost::none;
        }

    private:
        const std::vector<ViewableIntegerKeyedContainer::Entry>& _entries;
        boost::optional<int64_t> _position;
    };

    ViewableIntegerKeyedContainer() = default;

    explicit ViewableIntegerKeyedContainer(std::shared_ptr<Ident> ident)
        : _ident(std::move(ident)) {}

    std::shared_ptr<Ident> ident() const override {
        return _ident;
    }

    void setIdent(std::shared_ptr<Ident> ident) override {
        _ident = std::move(ident);
    }

    Status insert(RecoveryUnit&,
                  int64_t key,
                  std::span<const char> value,
                  container::ExistingKeyPolicy policy) override {
        _entries.emplace_back(key, std::string(value.begin(), value.end()));
        return Status::OK();
    }

    Status update(RecoveryUnit&, int64_t key, std::span<const char> value) override {
        auto it = std::find_if(_entries.begin(), _entries.end(), [key](const Entry& entry) {
            return entry.first == key;
        });
        it->second = std::string(value.begin(), value.end());
        return Status::OK();
    }

    Status remove(RecoveryUnit&, int64_t key) override {
        _entries.erase(std::find_if(_entries.begin(), _entries.end(), [key](const Entry& entry) {
            return entry.first == key;
        }));
        return Status::OK();
    }

    std::unique_ptr<IntegerKeyedContainer::Cursor> getCursor(RecoveryUnit&) const override {
        return std::make_unique<Cursor>(_entries);
    }

    const std::vector<Entry>& entries() const {
        return _entries;
    }

private:
    std::shared_ptr<Ident> _ident;
    std::vector<Entry> _entries;
};

}  // namespace mongo::sorter
