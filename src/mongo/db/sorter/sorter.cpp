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

/**
 * This is the implementation for the Sorter.
 *
 * It is intended to be included in other cpp files like this:
 *
 * #include <normal/include/files.h>
 *
 * #include "mongo/db/sorter/sorter.h"
 *
 * namespace mongo {
 *     // Your code
 * }
 *
 * #include "mongo/db/sorter/sorter.cpp"
 * MONGO_CREATE_SORTER(MyKeyType, MyValueType, MyComparatorType);
 *
 * Do this once for each unique set of parameters to MONGO_CREATE_SORTER.
 */

#include "mongo/db/sorter/sorter.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/util/atomic_int.h"
#include "mongo/db/cmdline.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/paths.h"

namespace mongo {
    namespace sorter {
        using namespace mongoutils;

        // We need to use the "real" errno everywhere, not GetLastError() on Windows
        inline std::string myErrnoWithDescription() {
            int errnoCopy = errno;
            StringBuilder sb;
            sb << "errno:" << errnoCopy << ' ' << strerror(errnoCopy);
            return sb.str();
        }

        template<typename Data, typename Comparator>
        void compIsntSane(const Comparator& comp, const Data& lhs, const Data& rhs) {
            PRINT(typeid(comp).name());
            PRINT(lhs.first);
            PRINT(lhs.second);
            PRINT(rhs.first);
            PRINT(rhs.second);
            PRINT(comp(lhs, rhs));
            PRINT(comp(rhs, lhs));
            dassert(false);
        }

        template<typename Data, typename Comparator>
        void dassertCompIsSane(const Comparator& comp, const Data& lhs, const Data& rhs) {
#if defined(_DEBUG) && !defined(_MSC_VER)
            // MSVC++ already does similar verification in debug mode in addition to using
            // algorithms that do more comparisons. Doing our own verification in addition makes
            // debug builds considerably slower without any additional safety.

            // test reversed comparisons
            const int regular = comp(lhs, rhs);
            if (regular == 0) {
                if (!(comp(rhs, lhs) == 0)) compIsntSane(comp, lhs, rhs);
            } else if (regular < 0) {
                if (!(comp(rhs, lhs) > 0)) compIsntSane(comp, lhs, rhs);
            } else /*regular > 0*/ {
                if (!(comp(rhs, lhs) < 0)) compIsntSane(comp, lhs, rhs);
            }

            // test reflexivity
            if (!(comp(lhs, lhs) == 0)) compIsntSane(comp, lhs, lhs);
            if (!(comp(rhs, rhs) == 0)) compIsntSane(comp, rhs, rhs);
#endif
        }

        /** Ensures a named file is deleted when this object goes out of scope */
        class FileDeleter {
        public:
            FileDeleter(const string& fileName) :_fileName(fileName) {}
            ~FileDeleter() { boost::filesystem::remove(_fileName); }
        private:
            const std::string _fileName;
        };

        /** Returns results from sorted in-memory storage */
        template <typename Key, typename Value>
        class InMemIterator : public SortIteratorInterface<Key, Value> {
        public:
            typedef std::pair<Key, Value> Data;

            /// No data to iterate
            InMemIterator() {}

            /// Only a single value
            InMemIterator(const Data& singleValue) :_data(1, singleValue) {}

            /// Any number of values
            template <typename Container>
            InMemIterator(const Container& input) :_data(input.begin(), input.end()) {}

            bool more() { return !_data.empty(); }
            Data next() {
                Data out = _data.front();
                _data.pop_front();
                return out;
            }

        private:
            std::deque<Data> _data;
        };

        /** Returns results in order from a single file */
        template <typename Key, typename Value>
        class FileIterator : public SortIteratorInterface<Key, Value> {
        public:
            typedef std::pair<typename Key::SorterDeserializeSettings
                             ,typename Value::SorterDeserializeSettings
                             > Settings;
            typedef std::pair<Key, Value> Data;

            FileIterator(const string& fileName,
                         const Settings& settings,
                         shared_ptr<FileDeleter> fileDeleter)
                : _settings(settings)
                , _done(false)
                , _fileName(fileName)
                , _fileDeleter(fileDeleter)
                , _file(_fileName.c_str(), std::ios::in | std::ios::binary)
            {
                massert(16814, str::stream() << "error opening file \"" << _fileName << "\": "
                                             << myErrnoWithDescription(),
                        _file.good());

                massert(16815, str::stream() << "unexpected empty file: " << _fileName,
                        boost::filesystem::file_size(_fileName) != 0);
            }

            bool more() {
                if (!_done)
                    fillIfNeeded(); // may change _done
                return !_done;
            }

            Data next() {
                verify(!_done);
                fillIfNeeded();

                Data out;
                // Note: key must be read before value so can't pass directly to Data constructor
                out.first = Key::deserializeForSorter(*_reader, _settings.first);
                out.second = Value::deserializeForSorter(*_reader, _settings.second);
                return out;
            }

        private:
            void fillIfNeeded() {
                verify(!_done);

                if (!_reader || _reader->atEof())
                    fill();
            }

            void fill() {
                int32_t blockSize;
                read(&blockSize, sizeof(blockSize));
                if (_done) return;

                _buffer.reset(new char[blockSize]);
                read(_buffer.get(), blockSize);
                massert(16816, "file too short?", !_done);
                _reader.reset(new BufReader(_buffer.get(), blockSize));
            }

            // sets _done to true on EOF - asserts on any other error
            void read(void* out, size_t size) {
                _file.read(reinterpret_cast<char*>(out), size);
                if (!_file.good()) {
                    if (_file.eof()) {
                        _done = true;
                        return;
                    }

                    msgasserted(16817, str::stream() << "error reading file \""
                                                     << _fileName << "\": "
                                                     << myErrnoWithDescription());
                }
                verify(_file.gcount() == static_cast<std::streamsize>(size));
            }

            const Settings _settings;
            bool _done;
            boost::scoped_array<char> _buffer;
            boost::scoped_ptr<BufReader> _reader;
            string _fileName;
            boost::shared_ptr<FileDeleter> _fileDeleter; // Must outlive _file
            ifstream _file;
        };

        /** Merge-sorts results from 0 or more FileIterators */
        template <typename Key, typename Value, typename Comparator>
        class MergeIterator : public SortIteratorInterface<Key, Value> {
        public:
            typedef SortIteratorInterface<Key, Value> Input;
            typedef std::pair<Key, Value> Data;


            MergeIterator(const vector<shared_ptr<Input> >& iters,
                          const SortOptions& opts,
                          const Comparator& comp)
                : _opts(opts)
                , _remaining(opts.limit ? opts.limit : numeric_limits<unsigned long long>::max())
                , _first(true)
                , _greater(comp)
            {
                for (size_t i = 0; i < iters.size(); i++) {
                    if (iters[i]->more()) {
                        _heap.push_back(
                            boost::make_shared<Stream>(i, iters[i]->next(), iters[i]));
                    }
                }

                if (_heap.empty()) {
                    _remaining = 0;
                    return;
                }

                std::make_heap(_heap.begin(), _heap.end(), _greater);
                std::pop_heap(_heap.begin(), _heap.end(), _greater);
                _current = _heap.back();
                _heap.pop_back();
            }

            bool more() {
                if (_remaining > 0 && (_first || !_heap.empty() || _current->more()))
                    return true;

                // We are done so clean up resources.
                // Can't do this in next() due to lifetime guarantees of unowned Data.
                _heap.clear();
                _current.reset();
                _remaining = 0;

                return false;
            }

            Data next() {
                verify(_remaining);

                _remaining--;

                if (_first) {
                    _first = false;
                    return _current->current();
                }

                if (!_current->advance()) {
                    verify(!_heap.empty());

                    std::pop_heap(_heap.begin(), _heap.end(), _greater);
                    _current = _heap.back();
                    _heap.pop_back();
                } else if (!_heap.empty() && _greater(_current, _heap.front())) {
                    std::pop_heap(_heap.begin(), _heap.end(), _greater);
                    std::swap(_current, _heap.back());
                    std::push_heap(_heap.begin(), _heap.end(), _greater);
                }

                return _current->current();
            }


        private:
            class Stream { // Data + Iterator
            public:
                Stream(size_t fileNum, const Data& first, boost::shared_ptr<Input> rest)
                    : fileNum(fileNum)
                    , _current(first)
                    , _rest(rest)
                {}

                const Data& current() const { return _current; }
                bool more() { return _rest->more(); }
                bool advance() {
                    if (!_rest->more())
                        return false;

                    _current = _rest->next();
                    return true;
                }

                const size_t fileNum;
            private:
                Data _current;
                boost::shared_ptr<Input> _rest;
            };

            class STLComparator { // uses greater rather than less-than to maintain a MinHeap
            public:
                explicit STLComparator(const Comparator& comp) : _comp(comp) {}
                bool operator () (ptr<const Stream> lhs, ptr<const Stream> rhs) const {
                    // first compare data
                    dassertCompIsSane(_comp, lhs->current(), rhs->current());
                    int ret = _comp(lhs->current(), rhs->current());
                    if (ret)
                        return ret > 0;

                    // then compare fileNums to ensure stability
                    return lhs->fileNum > rhs->fileNum;
                }
            private:
                const Comparator _comp;
            };

            SortOptions _opts;
            unsigned long long _remaining;
            bool _first;
            boost::shared_ptr<Stream> _current;
            std::vector<boost::shared_ptr<Stream> > _heap; // MinHeap
            STLComparator _greater; // named so calls make sense
        };

        template <typename Key, typename Value, typename Comparator>
        class NoLimitSorter : public Sorter<Key, Value> {
        public:
            typedef std::pair<Key, Value> Data;
            typedef SortIteratorInterface<Key, Value> Iterator;
            typedef std::pair<typename Key::SorterDeserializeSettings
                             ,typename Value::SorterDeserializeSettings
                             > Settings;

            NoLimitSorter(const SortOptions& opts,
                          const Comparator& comp,
                          const Settings& settings = Settings())
                : _comp(comp)
                , _settings(settings)
                , _opts(opts)
                , _memUsed(0)
            { verify(_opts.limit == 0); }

            void add(const Key& key, const Value& val) {
                _data.push_back(std::make_pair(key, val));

                _memUsed += key.memUsageForSorter();
                _memUsed += val.memUsageForSorter();

                if (_memUsed > _opts.maxMemoryUsageBytes)
                    spill();
            }

            Iterator* done() {
                if (_iters.empty()) {
                    sort();
                    return new InMemIterator<Key, Value>(_data);
                }

                spill();
                return Iterator::merge(_iters, _opts, _comp);
            }

            // TEMP these are here for compatibility. Will be replaced with a general stats API
            int numFiles() const { return _iters.size(); }
            size_t memUsed() const { return _memUsed; }

        private:
            class STLComparator {
            public:
                explicit STLComparator(const Comparator& comp) : _comp(comp) {}
                bool operator () (const Data& lhs, const Data& rhs) const {
                    dassertCompIsSane(_comp, lhs, rhs);
                    return _comp(lhs, rhs) < 0;
                }
            private:
                const Comparator& _comp;
            };

            void sort() {
                STLComparator less(_comp);
                std::stable_sort(_data.begin(), _data.end(), less);

                // Does 2x more compares than stable_sort
                // TODO test on windows
                //std::sort(_data.begin(), _data.end(), comp);
            }

            void spill() {
                if (_data.empty())
                    return;

                if (!_opts.extSortAllowed)
                    uasserted(16819, str::stream()
                        << "Sort exceeded memory limit of " << _opts.maxMemoryUsageBytes
                        << " bytes, but did not opt-in to external sorting. Aborting operation."
                        );

                sort();

                SortedFileWriter<Key, Value> writer(_settings);
                for ( ; !_data.empty(); _data.pop_front()) {
                    writer.addAlreadySorted(_data.front().first, _data.back().second);
                }

                _iters.push_back(boost::shared_ptr<Iterator>(writer.done()));

                _memUsed = 0;
            }

            const Comparator _comp;
            const Settings _settings;
            SortOptions _opts;
            size_t _memUsed;
            std::deque<Data> _data; // the "current" data
            std::vector<boost::shared_ptr<Iterator> > _iters; // data that has already been spilled
        };

        template <typename Key, typename Value, typename Comparator>
        class LimitOneSorter : public Sorter<Key, Value> {
            // Since this class is only used for limit==1, it omits all logic to
            // spill to disk and only tracks memory usage if explicitly requested.
        public:
            typedef std::pair<Key, Value> Data;
            typedef SortIteratorInterface<Key, Value> Iterator;

            LimitOneSorter(const SortOptions& opts, const Comparator& comp)
                : _comp(comp)
                , _haveData(false)
            { verify(opts.limit == 1); }

            void add(const Key& key, const Value& val) {
                Data contender(key, val);

                if (_haveData) {
                    dassertCompIsSane(_comp, _best, contender);
                    if (_comp(_best, contender) <= 0)
                        return; // not good enough
                } else {
                    _haveData = true;
                }

                _best = contender;
            }

            Iterator* done() {
                if (_haveData) {
                    return new InMemIterator<Key, Value>(_best);
                } else {
                    return new InMemIterator<Key, Value>();
                }
            }

            // TEMP these are here for compatibility. Will be replaced with a general stats API
            int numFiles() const { return 0; }
            size_t memUsed() const { return _best.first.memUsageForSorter()
                                          + _best.second.memUsageForSorter(); }

        private:
            const Comparator _comp;
            Data _best;
            bool _haveData; // false at start, set to true on first call to add()
        };

        template <typename Key, typename Value, typename Comparator>
        class TopKSorter : public Sorter<Key, Value> {
        public:
            typedef std::pair<Key, Value> Data;
            typedef SortIteratorInterface<Key, Value> Iterator;
            typedef std::pair<typename Key::SorterDeserializeSettings
                             ,typename Value::SorterDeserializeSettings
                             > Settings;

            TopKSorter(const SortOptions& opts,
                       const Comparator& comp,
                       const Settings& settings = Settings())
                : _comp(comp)
                , _settings(settings)
                , _opts(opts)
                , _memUsed(0)
            {
                // This also *works* with limit==1 but LimitOneSorter should be used instead
                verify(_opts.limit > 1);

                // Preallocate a fixed sized vector of the required size if we
                // don't expect it to have a major impact on our memory budget.
                // This is the common case with small limits.
                if ((sizeof(Data) * opts.limit) < opts.maxMemoryUsageBytes / 10) {
                    _data.reserve(opts.limit);
                }
            }

            void add(const Key& key, const Value& val) {
                STLComparator less(_comp);

                if (_data.size() < _opts.limit) {
                    _data.push_back(std::make_pair(key, val));

                    _memUsed += key.memUsageForSorter();
                    _memUsed += val.memUsageForSorter();

                    if (_data.size() == _opts.limit)
                        std::make_heap(_data.begin(), _data.end(), less);

                    if (_memUsed > _opts.maxMemoryUsageBytes)
                        spill();

                    return;
                }

                verify(_data.size() == _opts.limit);

                Data contender(key, val);
                if (!less(contender, _data.front()))
                    return; // not good enough

                // Remove the old worst pair and insert the contender, adjusting _memUsed

                _memUsed += key.memUsageForSorter();
                _memUsed += val.memUsageForSorter();

                _memUsed -= _data.front().first.memUsageForSorter();
                _memUsed -= _data.front().second.memUsageForSorter();

                std::pop_heap(_data.begin(), _data.end(), less);
                _data.back() = contender;
                std::push_heap(_data.begin(), _data.end(), less);

                if (_memUsed > _opts.maxMemoryUsageBytes)
                    spill();
            }

            Iterator* done() {
                if (_iters.empty()) {
                    sort();
                    return new InMemIterator<Key, Value>(_data);
                }

                spill();
                return Iterator::merge(_iters, _opts, _comp);
            }

            // TEMP these are here for compatibility. Will be replaced with a general stats API
            int numFiles() const { return _iters.size(); }
            size_t memUsed() const { return _memUsed; }

        private:
            class STLComparator {
            public:
                explicit STLComparator(const Comparator& comp) : _comp(comp) {}
                bool operator () (const Data& lhs, const Data& rhs) const {
                    dassertCompIsSane(_comp, lhs, rhs);
                    return _comp(lhs, rhs) < 0;
                }
            private:
                const Comparator& _comp;
            };

            void sort() {
                STLComparator less(_comp);

                if (_data.size() == _opts.limit) {
                    std::sort_heap(_data.begin(), _data.end(), less);
                } else {
                    std::stable_sort(_data.begin(), _data.end(), less);
                }
            }

            void spill() {
                if (_data.empty())
                    return;

                if (!_opts.extSortAllowed)
                    uasserted(16820, str::stream()
                        << "Sort exceeded memory limit of " << _opts.maxMemoryUsageBytes
                        << " bytes, but did not opt-in to external sorting. Aborting operation."
                        );

                sort();

                SortedFileWriter<Key, Value> writer(_settings);
                for (size_t i=0; i<_data.size(); i++) {
                    writer.addAlreadySorted(_data[i].first, _data[i].second);
                }

                // clear _data and release backing array's memory
                std::vector<Data>().swap(_data);

                _iters.push_back(boost::shared_ptr<Iterator>(writer.done()));

                _memUsed = 0;
            }

            const Comparator _comp;
            const Settings _settings;
            SortOptions _opts;
            size_t _memUsed;
            std::vector<Data> _data; // the "current" data. Organized as max-heap if size == limit.
            std::vector<boost::shared_ptr<Iterator> > _iters; // data that has already been spilled
        };

        inline unsigned nextFileNumber() {
            // This is unified across all Sorter types and instances.
            static AtomicUInt fileCounter;
            return fileCounter++;
        }
    } // namespace sorter

    //
    // SortedFileWriter
    //


    template <typename Key, typename Value>
    SortedFileWriter<Key, Value>::SortedFileWriter(const Settings& settings)
        : _settings(settings)
    {
        // This should be checked by consumers, but if we get here don't allow writes.
        massert(16946, "Attempting to use external sort from mongos. This is not allowed.",
                !cmdLine.isMongos());

        {
            StringBuilder sb;
            // TODO use tmpPath rather than dbpath/_tmp
            sb << dbpath << "/_tmp" << "/extsort." << sorter::nextFileNumber();
            _fileName = sb.str();
        }

        boost::filesystem::create_directories(dbpath + "/_tmp/");

        _file.open(_fileName.c_str(), ios::binary | ios::out);
        massert(16818, str::stream() << "error opening file \"" << _fileName << "\": "
                                     << sorter::myErrnoWithDescription(),
                _file.good());

        _fileDeleter = boost::make_shared<sorter::FileDeleter>(_fileName);

        // throw on failure
        _file.exceptions(ios::failbit | ios::badbit | ios::eofbit);
    }

    template <typename Key, typename Value>
    void SortedFileWriter<Key, Value>::addAlreadySorted(const Key& key, const Value& val) {
        key.serializeForSorter(_buffer);
        val.serializeForSorter(_buffer);

        if (_buffer.len() > 64*1024)
            spill();
    }

    template <typename Key, typename Value>
    void SortedFileWriter<Key, Value>::spill() {
        const int32_t size = _buffer.len();
        if (size == 0)
            return;

        try {
            _file.write(reinterpret_cast<const char*>(&size), sizeof(size));
            _file.write(_buffer.buf(), size);
        } catch (const std::exception&) {
            msgasserted(16821, str::stream() << "error writing to file \"" << _fileName << "\": "
                                             << sorter::myErrnoWithDescription());
        }

        _buffer.reset();
    }

    template <typename Key, typename Value>
    SortIteratorInterface<Key, Value>* SortedFileWriter<Key, Value>::done() {
        spill();
        _file.close();
        return new sorter::FileIterator<Key, Value>(_fileName, _settings, _fileDeleter);
    }

    //
    // Factory Functions
    //

    template <typename Key, typename Value>
    template <typename Comparator>
    SortIteratorInterface<Key, Value>* SortIteratorInterface<Key, Value>::merge(
            const std::vector<boost::shared_ptr<SortIteratorInterface> >& iters,
            const SortOptions& opts,
            const Comparator& comp) {
        return new sorter::MergeIterator<Key, Value, Comparator>(iters, opts, comp);
    }

    template <typename Key, typename Value>
    template <typename Comparator>
    Sorter<Key, Value>* Sorter<Key, Value>::make(const SortOptions& opts,
                                                 const Comparator& comp,
                                                 const Settings& settings) {

        // This should be checked by consumers, but if it isn't try to fail early.
        massert(16947, "Attempting to use external sort from mongos. This is not allowed.",
                !(cmdLine.isMongos() && opts.extSortAllowed));

        switch (opts.limit) {
            case 0:  return new sorter::NoLimitSorter<Key, Value, Comparator>(opts, comp, settings);
            case 1:  return new sorter::LimitOneSorter<Key, Value, Comparator>(opts, comp);
            default: return new sorter::TopKSorter<Key, Value, Comparator>(opts, comp, settings);
        }
    }
}
