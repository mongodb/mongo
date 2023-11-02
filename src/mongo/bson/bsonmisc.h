/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/util/builder.h"

namespace mongo {

class BSONElementCmpWithoutField {
public:
    /**
     * If 'stringComparator' is null, the default binary comparator will be used for comparing
     * string elements.  A custom string comparator may be provided, but it must outlive the
     * constructed BSONElementCmpWithoutField.
     */
    BSONElementCmpWithoutField(const StringDataComparator* stringComparator = nullptr)
        : _stringComparator(stringComparator) {}

    bool operator()(const BSONElement& l, const BSONElement& r) const {
        return l.woCompare(r, false, _stringComparator) < 0;
    }

private:
    const StringDataComparator* _stringComparator;
};

/** Use BSON macro to build a BSONObj from a stream

    e.g.,
       BSON( "name" << "joe" << "age" << 33 )

    with auto-generated object id:
       BSON( GENOID << "name" << "joe" << "age" << 33 )

    The labels GT, GTE, LT, LTE, NE can be helpful for stream-oriented construction
    of a BSONObj, particularly when assembling a Query.  For example,
    BSON( "a" << GT << 23.4 << NE << 30 << "b" << 2 ) produces the object
    { a: { \$gt: 23.4, \$ne: 30 }, b: 2 }.
*/
#define BSON(x) ((::mongo::BSONObjBuilder(64) << x).obj())

/** Use BSON_ARRAY macro like BSON macro, but without keys

    BSONArray arr = BSON_ARRAY( "hello" << 1 <<
                        BSON( "foo" << BSON_ARRAY( "bar" << "baz" << "qux" ) ) );

 */
#define BSON_ARRAY(x) ((::mongo::BSONArrayBuilder() << x).arr())

/* Utility class to auto assign object IDs.
   Example:
     std::cout << BSON( GENOID << "z" << 3 ); // { _id : ..., z : 3 }
*/
struct GENOIDLabeler {};
extern GENOIDLabeler GENOID;

/* Utility class to add a Date element with the current time
   Example:
     std::cout << BSON( "created" << DATENOW ); // { created : "2009-10-09 11:41:42" }
*/
struct DateNowLabeler {};
extern DateNowLabeler DATENOW;

/* Utility class to assign a NULL value to a given attribute
   Example:
     std::cout << BSON( "a" << BSONNULL ); // { a : null }
*/
struct NullLabeler {};
extern NullLabeler BSONNULL;

/* Utility class to assign an Undefined value to a given attribute
   Example:
     std::cout << BSON( "a" << BSONUndefined ); // { a : undefined }
*/
struct UndefinedLabeler {};
extern UndefinedLabeler BSONUndefined;

/* Utility class to add the minKey (minus infinity) to a given attribute
   Example:
     std::cout << BSON( "a" << MINKEY ); // { "a" : { "$minKey" : 1 } }
*/
struct MinKeyLabeler {};
extern MinKeyLabeler MINKEY;
struct MaxKeyLabeler {};
extern MaxKeyLabeler MAXKEY;

// Utility class to implement GT, GTE, etc as described above.
class Labeler {
public:
    struct Label {
        explicit Label(const char* l) : l_(l) {}
        const char* l_;
    };
    Labeler(const Label& l, BSONObjBuilderValueStream* s) : l_(l), s_(s) {}
    template <class T>
    BSONObjBuilder& operator<<(T value);

    /* the value of the element e is appended i.e. for
         "age" << GT << someElement
       one gets
         { age : { $gt : someElement's value } }
    */
    BSONObjBuilder& operator<<(const BSONElement& e);

private:
    const Label& l_;
    BSONObjBuilderValueStream* s_;
};

extern Labeler::Label GT;
extern Labeler::Label GTE;
extern Labeler::Label LT;
extern Labeler::Label LTE;
extern Labeler::Label NE;
extern Labeler::Label NIN;
extern Labeler::Label BSIZE;

// definitions in bsonobjbuilder.h b/c of incomplete types

// Utility class to implement BSON( key << val ) as described above.
class BSONObjBuilderValueStream {
    BSONObjBuilderValueStream(const BSONObjBuilderValueStream&) = delete;
    BSONObjBuilderValueStream& operator=(const BSONObjBuilderValueStream&) = delete;

public:
    friend class Labeler;
    BSONObjBuilderValueStream(BSONObjBuilder* builder);

    BSONObjBuilder& operator<<(const BSONElement& e);

    template <class T>
    BSONObjBuilder& operator<<(T value);

    BSONObjBuilder& operator<<(const DateNowLabeler& id);

    BSONObjBuilder& operator<<(const NullLabeler& id);
    BSONObjBuilder& operator<<(const UndefinedLabeler& id);

    BSONObjBuilder& operator<<(const MinKeyLabeler& id);
    BSONObjBuilder& operator<<(const MaxKeyLabeler& id);

    Labeler operator<<(const Labeler::Label& l);

    void endField(StringData nextFieldName = StringData());
    bool subobjStarted() const {
        return _fieldName != nullptr;
    }

    // The following methods provide API compatibility with BSONArrayBuilder
    BufBuilder& subobjStart();
    BufBuilder& subarrayStart();

    // This method should only be called from inside of implementations of
    // BSONObjBuilder& operator<<(BSONObjBuilderValueStream&, SOME_TYPE)
    // to provide the return value.
    BSONObjBuilder& builder() {
        return *_builder;
    }

    /**
     * Restores this object to its empty state.
     */
    void reset();

private:
    StringData _fieldName;
    BSONObjBuilder* _builder;

    bool haveSubobj() const {
        return _subobj.get() != nullptr;
    }
    BSONObjBuilder* subobj();
    std::unique_ptr<BSONObjBuilder> _subobj;
};

/**
   used in conjuction with BSONObjBuilder, allows for proper buffer size to prevent crazy memory
   usage
 */
class BSONSizeTracker {
public:
    BSONSizeTracker() {
        _pos = 0;
        for (int i = 0; i < SIZE; i++)
            _sizes[i] = 512;  // this is the default, so just be consistent
    }

    ~BSONSizeTracker() {}

    void got(int size) {
        _sizes[_pos] = size;
        _pos = (_pos + 1) % SIZE;  // thread safe at least on certain compilers
    }

    /**
     * right now choosing largest size
     */
    int getSize() const {
        int x = 16;  // sane min
        for (int i = 0; i < SIZE; i++) {
            if (_sizes[i] > x)
                x = _sizes[i];
        }
        return x;
    }

private:
    enum { SIZE = 10 };
    int _pos;
    int _sizes[SIZE];
};

// considers order
bool fieldsMatch(const BSONObj& lhs, const BSONObj& rhs);
}  // namespace mongo
