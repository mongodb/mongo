/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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

    Status insert(RecoveryUnit&, int64_t key, std::span<const char> value) override {
        _entries.emplace_back(key, std::string(value.begin(), value.end()));
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

    std::shared_ptr<IntegerKeyedContainer::Cursor> getSharedCursor(RecoveryUnit&) const override {
        return std::make_shared<Cursor>(_entries);
    }

    const std::vector<Entry>& entries() const {
        return _entries;
    }

private:
    std::shared_ptr<Ident> _ident;
    std::vector<Entry> _entries;
};

}  // namespace mongo::sorter
