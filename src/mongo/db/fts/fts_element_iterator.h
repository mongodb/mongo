// fts_element_iterator.h

/**
*    Copyright (C) 2014 MongoDB Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/fts/fts_language.h"
#include "mongo/db/fts/fts_spec.h"

#include <map>
#include <stack>
#include <string>

namespace mongo {

namespace fts {

/**
 *  Encapsulates data fields returned by FTSElementIterator
 */
struct FTSIteratorValue {
    FTSIteratorValue(const char* text, const FTSLanguage* language, double weight)
        : _text(text), _language(language), _weight(weight), _valid(true) {}

    FTSIteratorValue() : _text(NULL), _language(), _weight(0.0), _valid(false) {}

    bool valid() const {
        return _valid;
    }

    const char* _text;
    const FTSLanguage* _language;
    double _weight;
    bool _valid;
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
