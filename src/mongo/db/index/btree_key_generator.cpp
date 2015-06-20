/**
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

// SortStage checks for this error code in order to informatively error when we try to sort keys
// with parallel arrays.
const int BtreeKeyGenerator::ParallelArraysCode = 10088;

namespace {

const BSONObj nullObj = BSON("" << BSONNULL);
const BSONElement nullElt = nullObj.firstElement();
const BSONObj undefinedObj = BSON("" << BSONUndefined);
const BSONElement undefinedElt = undefinedObj.firstElement();

}  // namespace

BtreeKeyGenerator::BtreeKeyGenerator(std::vector<const char*> fieldNames,
                                     std::vector<BSONElement> fixed,
                                     bool isSparse)
    : _fieldNames(fieldNames), _isSparse(isSparse), _fixed(fixed) {
    BSONObjBuilder nullKeyBuilder;
    for (size_t i = 0; i < fieldNames.size(); ++i) {
        nullKeyBuilder.appendNull("");
    }
    _nullKey = nullKeyBuilder.obj();

    _isIdIndex = fieldNames.size() == 1 && std::string("_id") == fieldNames[0];
}

void BtreeKeyGenerator::getKeys(const BSONObj& obj, BSONObjSet* keys) const {
    if (_isIdIndex) {
        // we special case for speed
        BSONElement e = obj["_id"];
        if (e.eoo()) {
            keys->insert(_nullKey);
        } else {
            int size = e.size() + 5 /* bson over head*/ - 3 /* remove _id string */;
            BSONObjBuilder b(size);
            b.appendAs(e, "");
            keys->insert(b.obj());
            invariant(keys->begin()->objsize() == size);
        }
        return;
    }

    // '_fieldNames' and '_fixed' are passed by value so that they can be mutated as part of the
    // getKeys call.  :|
    getKeysImpl(_fieldNames, _fixed, obj, keys);
    if (keys->empty() && !_isSparse) {
        keys->insert(_nullKey);
    }
}

static void assertParallelArrays(const char* first, const char* second) {
    std::stringstream ss;
    ss << "cannot index parallel arrays [" << first << "] [" << second << "]";
    uasserted(BtreeKeyGenerator::ParallelArraysCode, ss.str());
}

BtreeKeyGeneratorV0::BtreeKeyGeneratorV0(std::vector<const char*> fieldNames,
                                         std::vector<BSONElement> fixed,
                                         bool isSparse)
    : BtreeKeyGenerator(fieldNames, fixed, isSparse) {}

void BtreeKeyGeneratorV0::getKeysImpl(std::vector<const char*> fieldNames,
                                      std::vector<BSONElement> fixed,
                                      const BSONObj& obj,
                                      BSONObjSet* keys) const {
    BSONElement arrElt;
    unsigned arrIdx = ~0;
    unsigned numNotFound = 0;

    for (unsigned i = 0; i < fieldNames.size(); ++i) {
        if (*fieldNames[i] == '\0')
            continue;

        BSONElement e = obj.getFieldDottedOrArray(fieldNames[i]);

        if (e.eoo()) {
            e = nullElt;  // no matching field
            numNotFound++;
        }

        if (e.type() != Array)
            fieldNames[i] = "";  // no matching field or non-array match

        if (*fieldNames[i] == '\0')
            // no need for further object expansion (though array expansion still possible)
            fixed[i] = e;

        if (e.type() == Array && arrElt.eoo()) {
            // we only expand arrays on a single path -- track the path here
            arrIdx = i;
            arrElt = e;
        }

        // enforce single array path here
        if (e.type() == Array && e.rawdata() != arrElt.rawdata()) {
            assertParallelArrays(e.fieldName(), arrElt.fieldName());
        }
    }

    bool allFound = true;  // have we found elements for all field names in the key spec?
    for (std::vector<const char*>::const_iterator i = fieldNames.begin(); i != fieldNames.end();
         ++i) {
        if (**i != '\0') {
            allFound = false;
            break;
        }
    }

    if (_isSparse && numNotFound == _fieldNames.size()) {
        // we didn't find any fields
        // so we're not going to index this document
        return;
    }

    bool insertArrayNull = false;

    if (allFound) {
        if (arrElt.eoo()) {
            // no terminal array element to expand
            BSONObjBuilder b(_sizeTracker);
            for (std::vector<BSONElement>::iterator i = fixed.begin(); i != fixed.end(); ++i)
                b.appendAs(*i, "");
            keys->insert(b.obj());
        } else {
            // terminal array element to expand, so generate all keys
            BSONObjIterator i(arrElt.embeddedObject());
            if (i.more()) {
                while (i.more()) {
                    BSONObjBuilder b(_sizeTracker);
                    for (unsigned j = 0; j < fixed.size(); ++j) {
                        if (j == arrIdx)
                            b.appendAs(i.next(), "");
                        else
                            b.appendAs(fixed[j], "");
                    }
                    keys->insert(b.obj());
                }
            } else if (fixed.size() > 1) {
                insertArrayNull = true;
            }
        }
    } else {
        // nonterminal array element to expand, so recurse
        verify(!arrElt.eoo());
        BSONObjIterator i(arrElt.embeddedObject());
        if (i.more()) {
            while (i.more()) {
                BSONElement e = i.next();
                if (e.type() == Object) {
                    getKeysImpl(fieldNames, fixed, e.embeddedObject(), keys);
                }
            }
        } else {
            insertArrayNull = true;
        }
    }

    if (insertArrayNull) {
        // x : [] - need to insert undefined
        BSONObjBuilder b(_sizeTracker);
        for (unsigned j = 0; j < fixed.size(); ++j) {
            if (j == arrIdx) {
                b.appendUndefined("");
            } else {
                BSONElement e = fixed[j];
                if (e.eoo())
                    b.appendNull("");
                else
                    b.appendAs(e, "");
            }
        }
        keys->insert(b.obj());
    }
}

BtreeKeyGeneratorV1::BtreeKeyGeneratorV1(std::vector<const char*> fieldNames,
                                         std::vector<BSONElement> fixed,
                                         bool isSparse)
    : BtreeKeyGenerator(fieldNames, fixed, isSparse), _emptyPositionalInfo(fieldNames.size()) {}

BSONElement BtreeKeyGeneratorV1::extractNextElement(const BSONObj& obj,
                                                    const PositionalPathInfo& positionalInfo,
                                                    const char** field,
                                                    bool* arrayNestedArray) const {
    std::string firstField = mongoutils::str::before(*field, '.');
    bool haveObjField = !obj.getField(firstField).eoo();
    BSONElement arrField = positionalInfo.positionallyIndexedElt;

    // An index component field name cannot exist in both a document
    // array and one of that array's children.
    uassert(16746,
            mongoutils::str::stream()
                << "Ambiguous field name found in array (do not use numeric field names in "
                   "embedded elements in an array), field: '" << arrField.fieldName()
                << "' for array: " << positionalInfo.arrayObj,
            !haveObjField || !positionalInfo.hasPositionallyIndexedElt());

    *arrayNestedArray = false;
    if (haveObjField) {
        return obj.getFieldDottedOrArray(*field);
    } else if (positionalInfo.hasPositionallyIndexedElt()) {
        if (arrField.type() == Array) {
            *arrayNestedArray = true;
        }
        *field = positionalInfo.remainingPath;
        return positionalInfo.dottedElt;
    }
    return BSONElement();
}

void BtreeKeyGeneratorV1::_getKeysArrEltFixed(
    std::vector<const char*>* fieldNames,
    std::vector<BSONElement>* fixed,
    const BSONElement& arrEntry,
    BSONObjSet* keys,
    unsigned numNotFound,
    const BSONElement& arrObjElt,
    const std::set<unsigned>& arrIdxs,
    bool mayExpandArrayUnembedded,
    const std::vector<PositionalPathInfo>& positionalInfo) const {
    // Set up any terminal array values.
    for (std::set<unsigned>::const_iterator j = arrIdxs.begin(); j != arrIdxs.end(); ++j) {
        unsigned idx = *j;
        if (*(*fieldNames)[idx] == '\0') {
            (*fixed)[idx] = mayExpandArrayUnembedded ? arrEntry : arrObjElt;
        }
    }

    // Recurse.
    getKeysImplWithArray(*fieldNames,
                         *fixed,
                         arrEntry.type() == Object ? arrEntry.embeddedObject() : BSONObj(),
                         keys,
                         numNotFound,
                         positionalInfo);
}

void BtreeKeyGeneratorV1::getKeysImpl(std::vector<const char*> fieldNames,
                                      std::vector<BSONElement> fixed,
                                      const BSONObj& obj,
                                      BSONObjSet* keys) const {
    getKeysImplWithArray(fieldNames, fixed, obj, keys, 0, _emptyPositionalInfo);
}

void BtreeKeyGeneratorV1::getKeysImplWithArray(
    std::vector<const char*> fieldNames,
    std::vector<BSONElement> fixed,
    const BSONObj& obj,
    BSONObjSet* keys,
    unsigned numNotFound,
    const std::vector<PositionalPathInfo>& positionalInfo) const {
    BSONElement arrElt;
    std::set<unsigned> arrIdxs;
    bool mayExpandArrayUnembedded = true;
    for (unsigned i = 0; i < fieldNames.size(); ++i) {
        if (*fieldNames[i] == '\0') {
            continue;
        }

        bool arrayNestedArray;
        // Extract element matching fieldName[ i ] from object xor array.
        BSONElement e =
            extractNextElement(obj, positionalInfo[i], &fieldNames[i], &arrayNestedArray);

        if (e.eoo()) {
            // if field not present, set to null
            fixed[i] = nullElt;
            // done expanding this field name
            fieldNames[i] = "";
            numNotFound++;
        } else if (e.type() == Array) {
            arrIdxs.insert(i);
            if (arrElt.eoo()) {
                // we only expand arrays on a single path -- track the path here
                arrElt = e;
            } else if (e.rawdata() != arrElt.rawdata()) {
                // enforce single array path here
                assertParallelArrays(e.fieldName(), arrElt.fieldName());
            }
            if (arrayNestedArray) {
                mayExpandArrayUnembedded = false;
            }
        } else {
            // not an array - no need for further expansion
            fixed[i] = e;
        }
    }

    if (arrElt.eoo()) {
        // No array, so generate a single key.
        if (_isSparse && numNotFound == fieldNames.size()) {
            return;
        }
        BSONObjBuilder b(_sizeTracker);
        for (std::vector<BSONElement>::iterator i = fixed.begin(); i != fixed.end(); ++i) {
            b.appendAs(*i, "");
        }
        keys->insert(b.obj());
    } else if (arrElt.embeddedObject().firstElement().eoo()) {
        // Empty array, so set matching fields to undefined.
        _getKeysArrEltFixed(&fieldNames,
                            &fixed,
                            undefinedElt,
                            keys,
                            numNotFound,
                            arrElt,
                            arrIdxs,
                            true,
                            _emptyPositionalInfo);
    } else {
        BSONObj arrObj = arrElt.embeddedObject();

        // For positional key patterns, e.g. {'a.1.b': 1}, we lookup the indexed array element
        // and then traverse the remainder of the field path up front. This prevents us from
        // having to look up the indexed element again on each recursive call (i.e. once per
        // array element).
        std::vector<PositionalPathInfo> subPositionalInfo(fixed.size());
        for (size_t i = 0; i < fieldNames.size(); ++i) {
            if (*fieldNames[i] == '\0') {
                // We've reached the end of the path.
                continue;
            }

            StringData part = fieldNames[i];
            part = part.substr(0, part.find('.'));
            subPositionalInfo[i].positionallyIndexedElt = arrObj[part];
            if (subPositionalInfo[i].positionallyIndexedElt.eoo()) {
                // Not indexing an array by position.
                continue;
            }

            // We're indexing an array element by its position. Traverse the remainder of the
            // field path now.
            subPositionalInfo[i].arrayObj = arrObj;
            subPositionalInfo[i].remainingPath = fieldNames[i];
            subPositionalInfo[i].dottedElt =
                arrObj.getFieldDottedOrArray(subPositionalInfo[i].remainingPath);
        }

        // Generate a key for each element of the indexed array.
        BSONObjIterator i(arrObj);
        while (i.more()) {
            _getKeysArrEltFixed(&fieldNames,
                                &fixed,
                                i.next(),
                                keys,
                                numNotFound,
                                arrElt,
                                arrIdxs,
                                mayExpandArrayUnembedded,
                                subPositionalInfo);
        }
    }
}
}  // namespace mongo
