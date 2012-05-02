// array.h

/*
 *    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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
