// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/util/modules.h"

#include <iosfwd>
#include <map>
#include <stack>
#include <string>
#include <string_view>

namespace mongo {

namespace fts {

/**
 *  Encapsulates data fields returned by FTSElementIterator
 */
class FTSIteratorValue {
public:
    FTSIteratorValue(std::string_view text, const FTSLanguage* language, double weight)
        : _text(text), _language(language), _weight(weight), _valid(true) {}

    FTSIteratorValue() = default;

    bool valid() const {
        return _valid;
    }

    std::string_view text() const {
        return _text;
    }

    const FTSLanguage* language() const {
        return _language;
    }

    double weight() const {
        return _weight;
    }

private:
    std::string_view _text;
    const FTSLanguage* _language = nullptr;
    double _weight = 0.0;
    bool _valid = false;
};

/**
 *  Iterator pattern for walking through text-indexed fields of a
 *  BSON document.
 *
 *  Example usage:
 *      FTSSpec spec( FTSSpec::fixSpec( indexSpec ) );
 *      FTSElementIterator it( spec, obj );
 *      while ( it.more() ) {
 *          FTSIteratorValue val = it.next();
 *          std::cout << val._text << '[' << val._language.str()
 *                                 << ',' << val._weight << ']' << std::endl;
 *      }
 *
 */
class FTSElementIterator {
public:
    /**
     *  Iterator constructor
     *
     *  Note: Caller must ensure that the constructed FTSElementIterator
     *          does >not< outlive either spec or obj.
     *
     *  @arg spec  text index specifier
     *  @arg obj   document that the iterator will traverse
     */
    FTSElementIterator(const FTSSpec& spec, const BSONObj& obj);

    /**
     *  Iterator interface: returns false iff there are no further text-indexable fields.
     */
    bool more();

    /**
     *  Iterator interface: advances to the next text-indexable field.
     */
    FTSIteratorValue next();

    /**
     *  Iterator frame needed for iterative implementation of
     *  recursive sub-documents.
     */
    struct FTSIteratorFrame {
        FTSIteratorFrame(const BSONObj& obj,
                         const FTSSpec& spec,
                         const FTSLanguage* parentLanguage,
                         const std::string& parentPath,
                         bool isArray)
            : _it(obj),
              _language(spec._getLanguageToUseV2(obj, parentLanguage)),
              _parentPath(parentPath),
              _isArray(isArray) {}

        friend std::ostream& operator<<(std::ostream&, FTSIteratorFrame&);

        BSONObjIterator _it;
        const FTSLanguage* _language;
        std::string _parentPath;
        bool _isArray;
    };

private:
    /**
     *  Helper method:
     *      returns false iff all FTSIteratorFrames on _frameStack are exhausted.
     */
    bool moreFrames();

    /**
     *  Helper method:
     *      advances to the next text-indexable field, possibly pushing frames as
     *      needed for recursive sub-documents.
     */
    FTSIteratorValue advance();

    /**
     *  Stack used by iterative implementation of recursive sub-document traversal.
     */
    std::stack<FTSIteratorFrame> _frameStack;

    /**
     *  Current frame, not yet pushed to stack.
     */
    FTSIteratorFrame _frame;

    /**
     *  Constructor input parameter: text index specification.
     */
    const FTSSpec& _spec;

    /**
     *  Current iterator return value, computed by 'more()', returned by 'next()'.
     */
    FTSIteratorValue _currentValue;
};

}  // namespace fts
}  // namespace mongo
