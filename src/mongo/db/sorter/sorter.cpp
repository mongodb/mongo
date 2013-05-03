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
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/paths.h"

namespace mongo {
    namespace sorter {
        using namespace mongoutils;

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
                                             << errnoWithDescription(),
                        _file.good());

                massert(16815, str::stream() << "unexpected empty file: " << _fileName,
                        boost::filesystem::file_size(_fileName) != 0);
            }

            bool more() {
                fillIfNeeded();
                return !_done;
            }

            Data next() {
                verify(!_done);

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
                                                     << errnoWithDescription());
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
                , _done(false)
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
                    _done = true;
                    return;
                }

                std::make_heap(_heap.begin(), _heap.end(), _greater);
                std::pop_heap(_heap.begin(), _heap.end(), _greater);
                _current = _heap.back();
                _heap.pop_back();
            }

            bool more() { return !_done && (_first || !_heap.empty() || _current->more()); }
            Data next() {
                verify(!_done);

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
                STLComparator(const Comparator& comp) : _comp(comp) {}
                bool operator () (ptr<const Stream> lhs, ptr<const Stream> rhs) const {
                    // first compare data
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
            bool _done;
            bool _first;
            boost::shared_ptr<Stream> _current;
            std::vector<boost::shared_ptr<Stream> > _heap; // MinHeap
            STLComparator _greater; // named so calls make sense
        };
    }

    template <typename Key, typename Value>
    template <typename Comparator>
    SortIteratorInterface<Key, Value>* SortIteratorInterface<Key, Value>::merge(
            const std::vector<boost::shared_ptr<SortIteratorInterface> >& iters,
            const SortOptions& opts,
            const Comparator& comp) {
        return new sorter::MergeIterator<Key, Value, Comparator>(iters, opts, comp);
    }

    //
    // SortedFileWriter
    //

    static AtomicUInt fileCounter;
    template <typename Key, typename Value>
    SortedFileWriter<Key, Value>::SortedFileWriter(const SortOptions& opts,
                                                   const Settings& settings)
        : _settings(settings)
        , _opts(opts)
    {
        {
            StringBuilder sb;
            // TODO use tmpPath rather than dbpath/_tmp
            sb << dbpath << "/_tmp" << "/extsort." << fileCounter++;
            _fileName = sb.str();
        }

        boost::filesystem::create_directories(dbpath + "/_tmp/");

        _file.open(_fileName.c_str(), ios::binary | ios::out);
        massert(16818, str::stream() << "error opening file \"" << _fileName << "\": "
                                 << errnoWithDescription(),
                _file.good());

        _fileDeleter = boost::make_shared<sorter::FileDeleter>(_fileName);

        // throw on failure
        _file.exceptions(ios::failbit | ios::badbit | ios::eofbit);
    }

    template <typename Key, typename Value>
    void SortedFileWriter<Key, Value>::addAlreadySorted(const Key& key, const Value& val) {
        BufBuilder bb;
        key.serializeForSorter(bb);
        val.serializeForSorter(bb);

        int32_t size = bb.len();
        _file.write(reinterpret_cast<const char*>(&size), sizeof(size));
        _file.write(bb.buf(), size);
    }

    template <typename Key, typename Value>
    SortIteratorInterface<Key, Value>* SortedFileWriter<Key, Value>::done() {
        _file.close();
        return new sorter::FileIterator<Key, Value>(_fileName, _settings, _fileDeleter);
    }

    //
    // Sorter
    //

    template <typename Key, typename Value, typename Comparator>
    Sorter<Key, Value, Comparator>::Sorter(const SortOptions& opts,
                                           const Comparator& comp,
                                           const Settings& settings)
        : _comp(comp)
        , _settings(settings)
        , _opts(opts)
        , _memUsed(0)
    {}

    template <typename Key, typename Value, typename Comparator>
    void Sorter<Key, Value, Comparator>::add(const Key& key, const Value& val) {
        _data.push_back(std::make_pair(key, val));

        _memUsed += key.memUsageForSorter();
        _memUsed += val.memUsageForSorter();

        if (_memUsed > _opts.maxMemoryUsageBytes)
            spill();
    }

    template <typename Key, typename Value, typename Comparator>
    SortIteratorInterface<Key, Value>* Sorter<Key, Value, Comparator>::done() {
        if (_iters.empty()) {
            sort();
            return new sorter::InMemIterator<Key, Value>(_data);
        }

        spill();
        return Iterator::merge(_iters, _opts, _comp);
    }

    template <typename Key, typename Value, typename Comparator>
    class Sorter<Key, Value, Comparator>::STLComparator {
    public:
        STLComparator(const Comparator& comp) : _comp(comp) {}
        bool operator () (const Data& lhs, const Data& rhs) const {
            return _comp(lhs, rhs) < 0;
        }
    private:
        const Comparator& _comp;
    };

    template <typename Key, typename Value, typename Comparator>
    void Sorter<Key, Value, Comparator>::sort() {
        STLComparator comp (_comp);
        std::stable_sort(_data.begin(), _data.end(), comp);
        //std::sort(_data.begin(), _data.end(), comp); // Does 2x more compares than stable_sort?
                                                       // TODO test on windows
    }

    template <typename Key, typename Value, typename Comparator>
    void Sorter<Key, Value, Comparator>::spill() {
        if (_data.empty())
            return;

        if (!_opts.extSortAllowed)
            uasserted(16819, str::stream()
                << "Sort exceeded memory limit of " << _opts.maxMemoryUsageBytes
                << " bytes, but did not opt-in to external sorting. Aborting operation."
                );

        sort();

        SortedFileWriter<Key, Value> writer(_opts, _settings);
        for ( ; !_data.empty(); _data.pop_front()) {
            writer.addAlreadySorted(_data.front().first, _data.back().second);
        }

        _iters.push_back(boost::shared_ptr<Iterator>(writer.done()));

        _memUsed = 0;
    }
}

/// The rest of this file is to ensure that if this file compiles the templates in it do.
#include <mongo/db/jsobj.h>
namespace mongo {
    class IntWrapper {
    public:
        IntWrapper(int i=0) :_i(i) {}
        operator const int& () const { return _i; }

        /// members for Sorter
        struct SorterDeserializeSettings {}; // unused
        void serializeForSorter(BufBuilder& buf) const { buf.appendNum(_i); }
        static IntWrapper deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
            return buf.read<int>();
        }
        int memUsageForSorter() const { return sizeof(IntWrapper); }
        IntWrapper getOwned() const { return *this; }
    private:
        int _i;
    };

    class BSONObj_DiskLoc_Sorter_Comparator {
    public:
        BSONObj_DiskLoc_Sorter_Comparator(const Ordering& ord) :_ord(ord) {}
        typedef std::pair<BSONObj, IntWrapper> Data;
        int operator() (const Data& l, const Data& r) const {
            int ret = l.first.woCompare(r.first, _ord);
            if (ret)
                return ret;

            if (l.second > r.second) return 1;
            if (l.second == r.second) return 0;
            return -1;
        }

    private:
        const Ordering _ord;
    };

    MONGO_CREATE_SORTER(BSONObj, IntWrapper, BSONObj_DiskLoc_Sorter_Comparator);
}


