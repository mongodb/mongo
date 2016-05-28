// fts_element_iterator.cpp
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

#include "mongo/db/fts/fts_element_iterator.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

#include <stack>

namespace mongo {

namespace fts {

using std::string;

extern const double DEFAULT_WEIGHT;
extern const double MAX_WEIGHT;

std::ostream& operator<<(std::ostream& os, FTSElementIterator::FTSIteratorFrame& frame) {
    BSONObjIterator it = frame._it;
    return os << "FTSIteratorFrame["
                 " element="
              << (*it).toString() << ", _language=" << frame._language->str()
              << ", _parentPath=" << frame._parentPath << ", _isArray=" << frame._isArray << "]";
}

FTSElementIterator::FTSElementIterator(const FTSSpec& spec, const BSONObj& obj)
    : _frame(obj, spec, &spec.defaultLanguage(), "", false),
      _spec(spec),
      _currentValue(advance()) {}

namespace {
/**  Check for exact match or path prefix match.  */
inline bool _matchPrefix(const string& dottedName, const string& weight) {
    if (weight == dottedName) {
        return true;
    }
    return mongoutils::str::startsWith(weight, dottedName + '.');
}
}

bool FTSElementIterator::more() {
    //_currentValue = advance();
    return _currentValue.valid();
}

FTSIteratorValue FTSElementIterator::next() {
    FTSIteratorValue result = _currentValue;
    _currentValue = advance();
    return result;
}

/**
 *  Helper method:
 *      if (current object iterator not exhausted) return true;
 *      while (frame stack not empty) {
 *          resume object iterator popped from stack;
 *          if (resumed iterator not exhausted) return true;
 *      }
 *      return false;
 */
bool FTSElementIterator::moreFrames() {
    if (_frame._it.more())
        return true;
    while (!_frameStack.empty()) {
        _frame = _frameStack.top();
        _frameStack.pop();
        if (_frame._it.more()) {
            return true;
        }
    }
    return false;
}

FTSIteratorValue FTSElementIterator::advance() {
    while (moreFrames()) {
        BSONElement elem = _frame._it.next();
        string fieldName = elem.fieldName();

        // Skip "language" specifier fields if wildcard.
        if (_spec.wildcard() && _spec.languageOverrideField() == fieldName) {
            continue;
        }

        // Compose the dotted name of the current field:
        // 1. parent path empty (top level): use the current field name
        // 2. parent path non-empty and obj is an array: use the parent path
        // 3. parent path non-empty and obj is a sub-doc: append field name to parent path
        string dottedName = (_frame._parentPath.empty() ? fieldName : _frame._isArray
                                     ? _frame._parentPath
                                     : _frame._parentPath + '.' + fieldName);

        // Find lower bound of dottedName in _weights.  lower_bound leaves us at the first
        // weight that could possibly match or be a prefix of dottedName.  And if this
        // element fails to match, then no subsequent weight can match, since the weights
        // are lexicographically ordered.
        Weights::const_iterator i =
            _spec.weights().lower_bound(elem.type() == Object ? dottedName + '.' : dottedName);

        // possibleWeightMatch is set if the weight map contains either a match or some item
        // lexicographically larger than fieldName.  This boolean acts as a guard on
        // dereferences of iterator 'i'.
        bool possibleWeightMatch = (i != _spec.weights().end());

        // Optimize away two cases, when not wildcard:
        // 1. lower_bound seeks to end(): no prefix match possible
        // 2. lower_bound seeks to a name which is not a prefix
        if (!_spec.wildcard()) {
            if (!possibleWeightMatch) {
                continue;
            } else if (!_matchPrefix(dottedName, i->first)) {
                continue;
            }
        }

        // Is the current field an exact match on a weight?
        bool exactMatch = (possibleWeightMatch && i->first == dottedName);
        double weight = (possibleWeightMatch ? i->second : DEFAULT_WEIGHT);

        switch (elem.type()) {
            case String:
                // Only index strings on exact match or wildcard.
                if (exactMatch || _spec.wildcard()) {
                    return FTSIteratorValue(elem.valuestr(), _frame._language, weight);
                }
                break;

            case Object:
                // Only descend into a sub-document on proper prefix or wildcard.  Note that
                // !exactMatch is a sufficient test for proper prefix match, because of
                //   if ( !matchPrefix( dottedName, i->first ) ) continue;
                // block above.
                if (!exactMatch || _spec.wildcard()) {
                    _frameStack.push(_frame);
                    _frame =
                        FTSIteratorFrame(elem.Obj(), _spec, _frame._language, dottedName, false);
                }
                break;

            case Array:
                // Only descend into arrays from non-array parents or on wildcard.
                if (!_frame._isArray || _spec.wildcard()) {
                    _frameStack.push(_frame);
                    _frame =
                        FTSIteratorFrame(elem.Obj(), _spec, _frame._language, dottedName, true);
                }
                break;

            default:
                // Skip over all other BSON types.
                break;
        }
    }
    return FTSIteratorValue();  // valid()==false
}

}  // namespace fts
}  // namespace mongo
