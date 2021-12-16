/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <boost/filesystem.hpp>

#include "mongo/db/sorter/spillable_sorter.h"

namespace mongo::sorter {
template <typename Key, typename Value>
class NoLimitSorter : public SpillableSorter<Key, Value> {
public:
    using Base = SpillableSorter<Key, Value>;
    using Data = typename Base::Data;
    using Iterator = typename Base::Iterator;
    using CompFn = typename Base::CompFn;
    using Settings = typename Base::Settings;

    using Base::_data;
    using Base::_done;
    using Base::_file;
    using Base::_less;
    using Base::_memUsed;
    using Base::_numSorted;
    using Base::_options;
    using Base::_settings;
    using Base::_spill;
    using Base::_spilled;
    using Base::_totalDataSizeSorted;

    NoLimitSorter(StringData name,
                  const Options& options,
                  const CompFn& comp,
                  const Settings& settings)
        : Base(name, options, comp, settings) {
        invariant(options.limit == 0);
    }

    NoLimitSorter(const std::string& fileName,
                  const std::vector<SorterRange>& ranges,
                  const Options& options,
                  const CompFn& comp,
                  const Settings& settings)
        : Base(options, comp, settings, fileName) {
        uassert(16815,
                str::stream() << "Unexpected empty file: " << _file->path().string(),
                ranges.empty() || boost::filesystem::file_size(_file->path()) != 0);

        _spilled.reserve(ranges.size());
        std::transform(ranges.begin(),
                       ranges.end(),
                       std::back_inserter(_spilled),
                       [this](const SorterRange& range) {
                           return std::make_unique<FileIterator<Key, Value>>(_file.get(),
                                                                             range.getStartOffset(),
                                                                             range.getEndOffset(),
                                                                             range.getChecksum(),
                                                                             _settings,
                                                                             _options.dbName);
                       });
    }

    void add(const Key& key, const Value& value) {
        addOwned(key.getOwned(), value.getOwned());
    }

    void addOwned(Key&& key, Value&& value) override {
        invariant(!_done);

        ++_numSorted;

        auto memUsage = key.memUsageForSorter() + value.memUsageForSorter();
        _memUsed += memUsage;
        _totalDataSizeSorted += memUsage;

        _data.emplace_back(std::move(key), std::move(value));

        if (_memUsed > _options.maxMemoryUsageBytes) {
            _spill();
        }
    }

    typename Base::PersistedState persistDataForShutdown() override {
        _spill();
        _file->keep();

        std::vector<SorterRange> ranges;
        ranges.reserve(_spilled.size());
        std::transform(_spilled.begin(),
                       _spilled.end(),
                       std::back_inserter(ranges),
                       [](const auto& it) { return it->getRange(); });

        return {_file->path().filename().string(), std::move(ranges)};
    }

private:
    void _sort() {
        std::stable_sort(_data.begin(), _data.end(), _less);
    }
};
}  // namespace mongo::sorter
