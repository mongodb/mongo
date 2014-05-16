// array.h

/*
 *    Copyright 2010 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

namespace mongo {

    /*
     * simple array class that does no allocations
     * same api as vector
     * fixed buffer, so once capacity is exceeded, will assert
     * meant to be-reused with clear()
     */
    template<typename T>
    class FastArray {
    public:
        FastArray( int capacity=10000 )
            : _capacity( capacity ) , _size(0) , _end(this,capacity) {
            _data = new T[capacity];
        }

        ~FastArray() {
            delete[] _data;
        }

        void clear() {
            _size = 0;
        }

        T& operator[]( int x ) {
            verify( x >= 0 && x < _capacity );
            return _data[x];
        }

        T& getNext() {
            return _data[_size++];
        }

        void push_back( const T& t ) {
            verify( _size < _capacity );
            _data[_size++] = t;
        }

        void sort( int (*comp)(const void *, const void *) ) {
            qsort( _data , _size , sizeof(T) , comp );
        }

        int size() {
            return _size;
        }

        bool hasSpace() {
            return _size < _capacity;
        }
        class iterator {
        public:
            iterator() {
                _it = 0;
                _pos = 0;
            }

            iterator( FastArray * it , int pos=0 ) {
                _it = it;
                _pos = pos;
            }

            bool operator==(const iterator& other ) const {
                return _pos == other._pos;
            }

            bool operator!=(const iterator& other ) const {
                return _pos != other._pos;
            }

            void operator++() {
                _pos++;
            }

            T& operator*() {
                return _it->_data[_pos];
            }

            string toString() const {
                stringstream ss;
                ss << _pos;
                return ss.str();
            }
        private:
            FastArray * _it;
            int _pos;

            friend class FastArray;
        };


        iterator begin() {
            return iterator(this);
        }

        iterator end() {
            _end._pos = _size;
            return _end;
        }


    private:
        int _capacity;
        int _size;

        iterator _end;

        T * _data;
    };
}
