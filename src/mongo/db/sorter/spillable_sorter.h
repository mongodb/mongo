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

#include "mongo/db/sorter/in_mem_iterator.h"
#include "mongo/db/sorter/merge_iterator.h"
#include "mongo/db/sorter/sorted_file_writer.h"
#include "mongo/db/sorter/sorter.h"

namespace mongo::sorter {
/**
 * A Sorter which may spill to disk if configured to do so. Each instance of this class will, if
 * spilling is enabled, generate a file name and spill sorted data ranges to that file.
 */
template <typename Key, typename Value>
class SpillableSorter : public Sorter<Key, Value> {
public:
    using Base = Sorter<Key, Value>;
    using Data = typename Base::Data;
    using Iterator = typename Base::Iterator;
    using CompFn = typename Base::CompFn;
    using Settings = typename Base::Settings;

    using Base::_comp;
    using Base::_done;

    SpillableSorter(StringData name,
                    const Options& options,
                    const CompFn& comp,
                    const Settings& settings)
        : SpillableSorter(options,
                          comp,
                          settings,
                          options.tempDir
                              ? std::make_unique<File>(*options.tempDir + "/" + nextFileName(name))
                              : nullptr) {}

    SpillableSorter(const Options& options,
                    const CompFn& comp,
                    const Settings& settings,
                    const std::string& fileName)
        : SpillableSorter(
              options, comp, settings, std::make_unique<File>(*options.tempDir + "/" + fileName)) {
        invariant(!fileName.empty());
    }

    std::unique_ptr<Iterator> done(typename Iterator::ReturnPolicy returnPolicy) {
        invariant(!std::exchange(_done, true));

        if (_spilled.empty()) {
            _sort();
            return std::make_unique<InMemIterator<Key, Value>>(_data, returnPolicy);
        }

        _spill();
        return std::make_unique<MergeIterator<Key, Value>>(_spilled, _options.limit, _comp);
    }

    size_t numSpills() const override {
        return _spilled.size();
    }

protected:
    virtual void _sort() = 0;

    void _spill() {
        if (_data.empty()) {
            return;
        }

        if (!_options.tempDir) {
            // This error message only applies to sorts from user queries made through the find or
            // aggregation commands. Other clients should suppress this error, either by allowing
            // external sorting or by catching and throwing a more appropriate error.
            uasserted(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
                      str::stream()
                          << "Sort exceeded memory limit of " << _options.maxMemoryUsageBytes
                          << " bytes, but did not opt in to external sorting.");
        }

        _sort();
        _updateStateAfterSort();

        SortedFileWriter<Key, Value> writer(_file.get(), _options.dbName, _settings);
        for (size_t i = 0; i < _data.size(); i++) {
            writer.addAlreadySorted(_data[i].first, _data[i].second);
        }
        _spilled.push_back(writer.done());

        // Clear _data and release backing array's memory.
        std::vector<Data>().swap(_data);
        _memUsed = 0;
    }

    virtual void _updateStateAfterSort() {}

    const Options _options;
    const std::function<bool(const Data&, const Data&)> _less;
    const Settings _settings;

    std::vector<Data> _data;

    std::unique_ptr<File> _file;
    std::vector<std::unique_ptr<Iterator>> _spilled;

    size_t _memUsed = 0;

private:
    SpillableSorter(const Options& options,
                    const CompFn& comp,
                    const Settings& settings,
                    std::unique_ptr<File> file)
        : Base(comp),
          _options(options),
          _less([comp](const Data& lhs, const Data& rhs) {
              dassertCompIsSane(comp, lhs, rhs);
              return comp(lhs, rhs) < 0;
          }),
          _settings(settings),
          _file(std::move(file)) {}
};
}  // namespace mongo::sorter
