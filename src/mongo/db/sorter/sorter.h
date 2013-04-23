/*
*    Copyright (C) 2013 10gen Inc.
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
*/

#pragma once

#include <boost/smart_ptr.hpp>
#include <deque>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <mongo/base/disallow_copying.h>

/**
 * This is the public API for the Sorter (both in-memory and external)
 */

namespace mongo {
    namespace sorter {
        // Everything in this namespace is internal to the sorter
        class FileDeleter;
    }

    /**
     * Runtime options that control the Sorter's behavior
     */
    struct SortOptions {
        long long limit; /// number of KV pairs to be returned. 0 for no limit.
        size_t maxMemoryUsageBytes; /// Approximate.
        bool extSortAllowed; /// If false, uassert if more mem needed than allowed.
        SortOptions()
            : limit(0)
            , maxMemoryUsageBytes(64*1024*1024)
            , extSortAllowed(false)
        {}
    };

    template <typename Key, typename Value>
    class SortIteratorInterface {
        MONGO_DISALLOW_COPYING(SortIteratorInterface);
    public:
        typedef std::pair<Key, Value> Data;

        // Unowned objects are only valid until next call to any method

        virtual bool more() =0;
        virtual std::pair<Key, Value> next() =0;

        virtual ~SortIteratorInterface() {}

        /// Returns an iterator that merges the passed in iterators
        template <typename Comparator>
        static SortIteratorInterface* merge(
                const std::vector<boost::shared_ptr<SortIteratorInterface> >& iters,
                const SortOptions& opts,
                const Comparator& comp);
    protected:
        SortIteratorInterface() {} // can only be constructed as a base
    };

    template <typename Key, typename Value, typename Comparator>
    class Sorter {
        MONGO_DISALLOW_COPYING(Sorter);
    public:
        typedef std::pair<Key, Value> Data;
        typedef SortIteratorInterface<Key, Value> Iterator;
        typedef std::pair<typename Key::SorterDeserializeSettings
                         ,typename Value::SorterDeserializeSettings
                         > Settings;

        explicit Sorter(const SortOptions& opts,
                        const Comparator& comp,
                        const Settings& settings = Settings());
        void add(const Key&, const Value&);
        Iterator* done();

        // TEMP these are here for compatibility. Will be replaced with a general stats API
        int numFiles() const { return _iters.size(); }
        size_t memUsed() const { return _memUsed; }
    private:
        class STLComparator;

        void sort();
        void spill();

        const Comparator _comp;
        const Settings _settings;
        SortOptions _opts;
        size_t _memUsed;
        std::deque<Data> _data; // the "current" data
        std::vector<boost::shared_ptr<Iterator> > _iters; // data that has already been spilled
    };


    template <typename Key, typename Value>
    class SortedFileWriter {
        MONGO_DISALLOW_COPYING(SortedFileWriter);
    public:
        typedef SortIteratorInterface<Key, Value> Iterator;
        typedef std::pair<typename Key::SorterDeserializeSettings
                         ,typename Value::SorterDeserializeSettings
                         > Settings;

        explicit SortedFileWriter(const SortOptions& opts, const Settings& = Settings());
        void addAlreadySorted(const Key&, const Value&);
        Iterator* done();

    private:
        const Settings _settings;
        SortOptions _opts;
        std::string _fileName;
        boost::shared_ptr<sorter::FileDeleter> _fileDeleter; // Must outlive _file
        std::ofstream _file;
    };
}

#define MONGO_CREATE_SORTER(Key, Value, Comparator) \
    template class ::mongo::Sorter<Key, Value, Comparator>; \
    template class ::mongo::SortIteratorInterface<Key, Value>; \
    template class ::mongo::SortedFileWriter<Key, Value>; \
    template class ::mongo::sorter::MergeIterator<Key, Value, Comparator>; \
    template class ::mongo::sorter::InMemIterator<Key, Value>; \
    template class ::mongo::sorter::FileIterator<Key, Value>; \
    template ::mongo::SortIteratorInterface<Key, Value>* \
                ::mongo::SortIteratorInterface<Key, Value>::merge<Comparator>( \
                    const std::vector<boost::shared_ptr<SortIteratorInterface> >& iters, \
                    const SortOptions& opts, \
                    const Comparator& comp);

