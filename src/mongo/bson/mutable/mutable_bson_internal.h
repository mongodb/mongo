/* Copyright 2010 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <vector>

#include "mongo/bson/mutable/mutable_bson.h"
#include "mongo/platform/cstdint.h"

namespace mongo {
namespace mutablebson {

/*
 * Internal representation for a mutable bson.
 *
 * Mutable BSON represents documents using buffers in a packed
 * non-serialized, mutable data structure. The layout is as follows:
 *
 *    heap: [ (en:32, [byte*)* ]
 *      [03|abc|03|def|04|ghij|... ]
 *
 * elementVector: [ ElementRep* ]
 *  [r0|r1|r2|r3|... ]
 *
 *
 * ElementReps are fixed-width data structures that include:
 *
 *        type:32,           // 32 bits, regular BSONType, additional bits usage tbd
 *        name:32,           // 32 bits, field name heap reference
 *        value:64           // 64 bits, either inline atomic or heap reference
 *         //overloaded as:
 *         leftChild:32      // 32 bits, ref to leftmost child
 *         rightChild:32     // 32 bits, ref to rightmost child
 *        rightSibling:32    // 32 bits, ref to next sibling
 *        leftSibling:32     // 32 bits, ref to prev sibling
 *        parent:32          // 32 bits, ref to parent
 *        pad to 32 bytes    // = minimal single cache line
 *
 *
 * ElementRep contains node data
 * Element wraps ElementRep by adding document, (.g) heap
 *
 * Element::find returns an Iterator
 * Iterators have undefined behavior after move or removal of current node or
 *   any ancestor of current node.
 *
 * Document includes the heap and element vector
 *
 * BasicHeap implements Heap interface using vector<uint8_t>
 * BSONObjHeap implements Heap interface using BSON _objdata buffer + vector<uint8_t>
 * Values of size <= 64 bits may be stored inline in ElementReps
 * Embedded objects and arrays are stored as elements with up/down-links
 *   to/from parent, and doubly linked sibling indexes.
 * Larger atomic types and variable-length types are stored in the heap
 *   and referenced via heap indexes.
 *
 * For example, the document
 *
 *   { aa:1, bbb:{c:2, d:{e:{f:"xyz", g:[4,5,6]}}} }
 *
 * is represented as follows:
 *
 * MutableBSON::ElementVector
 * 0 [top| 0| 1  2| .| .| .] {           root node   left/right children "aa", "bbb"
 * 1 [num|00| 0  1| 2| .| 0] "aa":1      leaf node   value = 1; right sibling "bbb"
 * 2 [obj|06| 3  4| .| 1| 0] "bbb":{..}  subtree     left/right children "c","d"; left sib "aa"
 * 3 [num|13| 0  2| 3| .| 2] "c":2       leaf node   value=2; right sibling "d"
 * 4 [obj|18| 5  5| .| 4| 2] "d":{..}    subtree     left/right children "e", "e"; left sib "c"
 * 5 [obj|23| 6  7| .| .| 4] "e":{..}    subtree     left/right children "f", "g"
 * 6 [str|28| 0 33| 7| .| 5] "f":"xyz"   leaf node   value on heap, right sibling "g"
 * 7 [obj|40| 8 10| .| 6| 5] "g":{..}    sublist     left/right children "g.0","g.2"; left sib "f"
 * 8 [num|45| 0  4| 9| .| 7] "0":4       leaf node   value=4; right sibling "g.1"
 * 9 [num|50| 0  5|10| 8| 7] "1":5       leaf node   value=5; right sibling "g.2", left sib "g.1"
 * 10[num|55| 0  6| .| 9| 7] "2":6       leaf node   value=6; left sibling "g.1"
 *            +  +
 *            |__|__ 64 bit value, or pair of 32-bit left/right child indexes
 *
 * Heap
 * [02|aa|03|bbb|01|c|01|d|01|e|01|f|03|xyz|01|g|01|0|01|1|01|2|.. ]
 *  ^     ^      ^    ^    ^    ^    ^      ^    ^    ^    ^
 *  0     6      13   18   23   28   33     40   45   50   55
 *
 */

    static const uint32_t EMPTY_REP = (uint32_t)-1;
    static const uint32_t NULL_REF  = (uint32_t)-1;
    static const uint32_t SHORT_LIMIT = 16;

    union ValueType {
        bool boolVal;
        int32_t intVal;
        int64_t longVal;
        int64_t tsVal;
        int64_t dateVal;
        double doubleVal;
        char shortStr[SHORT_LIMIT];  /* OID and short strings */
        uint64_t valueRef;  /* index to heap (strings, binary, regex) */
    };

    /**
        fixed-width node - stored in Document ElementVector 

	"rep" is a name coined by Stroustrup in his original
	std::string implementation. It means "representative".
        Element is the abstraction, ElementRep is the implementation class.
        Algorithms manipulate Elements.  ElementRep's can be swapped out
        without change upstairs.
     */
    struct ElementRep {
        ElementRep()
        : _type(mongo::Undefined)
        , _nameref(NULL_REF)
        , _parent(EMPTY_REP) {
            _child._left = EMPTY_REP;
            _child._right = EMPTY_REP;
            _sibling._left = EMPTY_REP;
            _sibling._right = EMPTY_REP;
        }

        ElementRep(int32_t type)
        : _type(type)
        , _nameref(NULL_REF)
        , _parent(EMPTY_REP) {
            _child._left = EMPTY_REP;
            _child._right = EMPTY_REP;
            _sibling._left = EMPTY_REP;
            _sibling._right = EMPTY_REP;
        }

        ElementRep(int32_t type, uint32_t nameref)
        : _type(type)
        , _nameref(nameref)
        , _parent(EMPTY_REP) {
            _child._left = EMPTY_REP;
            _child._right = EMPTY_REP;
            _sibling._left = EMPTY_REP;
            _sibling._right = EMPTY_REP;
        }

        ElementRep(int32_t type, uint32_t nameref, ValueType value)
        : _type(type)
        , _nameref(nameref)
        , _value(value)
        , _parent(EMPTY_REP) {
            _sibling._left = EMPTY_REP;
            _sibling._right = EMPTY_REP;
        }

        ElementRep(int32_t type, uint32_t nameref, ValueType value, uint32_t parentref)
        : _type(type)
        , _nameref(nameref)
        , _value(value)
        , _parent(parentref) {
            _sibling._left = EMPTY_REP;
            _sibling._right = EMPTY_REP;
        }

        void clearSiblings() {
            _sibling._left = EMPTY_REP;
            _sibling._right = EMPTY_REP;
        }

        void clearParent() {
            _parent = EMPTY_REP;
        }

        int32_t _type;
        uint32_t _nameref;

        union {
            ValueType _value;
            struct {
                uint32_t _left;
                uint32_t _right;
            } _child;
        };
        struct {
            uint32_t _left;
            uint32_t _right;
        } _sibling;
        uint32_t _parent;
    };


    /** node store - as vector of ElementRep's */
    class ElementVector {
    public:
        // vector interface
        void push_back(const ElementRep& elemRep) { _vec.push_back(elemRep); }
        ElementRep& operator[](uint32_t index) { return _vec[index]; }
        const ElementRep& operator[](uint32_t index) const { return _vec[index]; }
        uint32_t size() const { return _vec.size(); }

    private:
        friend class Element;
        friend class SiblingIterator;

        std::vector<ElementRep> _vec;
    };

} // namespace mutablebson
} // namespace mongo
