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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/query/type_explain.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    // TODO: This doesn't need to be so complicated or serializable.  Let's throw this out when we
    // move to explain V2

    using mongoutils::str::stream;

    const BSONField<std::vector<TypeExplain*> > TypeExplain::clauses("clauses");
    const BSONField<std::string> TypeExplain::cursor("cursor");
    const BSONField<bool> TypeExplain::isMultiKey("isMultiKey");
    const BSONField<long long> TypeExplain::n("n", 0);
    const BSONField<long long> TypeExplain::nScannedObjects("nscannedObjects", 0);
    const BSONField<long long> TypeExplain::nScanned("nscanned", 0);
    const BSONField<long long> TypeExplain::nScannedObjectsAllPlans("nscannedObjectsAllPlans");
    const BSONField<long long> TypeExplain::nScannedAllPlans("nscannedAllPlans");
    const BSONField<bool> TypeExplain::scanAndOrder("scanAndOrder");
    const BSONField<bool> TypeExplain::indexOnly("indexOnly");
    const BSONField<long long> TypeExplain::nYields("nYields");
    const BSONField<long long> TypeExplain::nChunkSkips("nChunkSkips");
    const BSONField<long long> TypeExplain::millis("millis");
    const BSONField<BSONObj> TypeExplain::indexBounds("indexBounds");
    const BSONField<std::vector<TypeExplain*> > TypeExplain::allPlans("allPlans");
    const BSONField<TypeExplain*> TypeExplain::oldPlan("oldPlan");
    const BSONField<bool> TypeExplain::indexFilterApplied("filterSet");
    const BSONField<std::string> TypeExplain::server("server");

    TypeExplain::TypeExplain() {
        clear();
    }

    TypeExplain::~TypeExplain() {
        unsetClauses();
        unsetAllPlans();
    }

    bool TypeExplain::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isNSet) {
            *errMsg = stream() << "missing " << n.name() << " field";
            return false;
        }

        if (!_isNScannedObjectsSet) {
            *errMsg = stream() << "missing " << nScannedObjects.name() << " field";
            return false;
        }

        if (!_isNScannedSet) {
            *errMsg = stream() << "missing " << nScanned.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj TypeExplain::toBSON() const {
        BSONObjBuilder builder;

        if (_clauses.get()) {
            BSONArrayBuilder clausesBuilder(builder.subarrayStart(clauses()));
            for (std::vector<TypeExplain*>::const_iterator it = _clauses->begin();
                 it != _clauses->end();
                 ++it) {
                BSONObj clausesDocument = (*it)->toBSON();
                clausesBuilder.append(clausesDocument);
            }
            clausesBuilder.done();
        }

        if (_isCursorSet) builder.append(cursor(), _cursor);

        if (_isIsMultiKeySet) builder.append(isMultiKey(), _isMultiKey);

        if (_isNSet) {
            builder.appendNumber(n(), _n);
        }
        else {
            builder.appendNumber(n(), n.getDefault());
        }

        if (_isNScannedObjectsSet) {
            builder.appendNumber(nScannedObjects(), _nScannedObjects);
        }
        else {
            builder.appendNumber(nScannedObjects(), nScannedObjects.getDefault());
        }

        if (_isNScannedSet) {
            builder.appendNumber(nScanned(), _nScanned);
        }
        else {
            builder.appendNumber(nScanned(), nScanned.getDefault());
        }

        if (_isNScannedObjectsAllPlansSet)
            builder.appendNumber(nScannedObjectsAllPlans(), _nScannedObjectsAllPlans);

        if (_isNScannedAllPlansSet) builder.appendNumber(nScannedAllPlans(), _nScannedAllPlans);

        if (_isScanAndOrderSet) builder.append(scanAndOrder(), _scanAndOrder);

        if (_isIndexOnlySet) builder.append(indexOnly(), _indexOnly);

        if (_isNYieldsSet) builder.appendNumber(nYields(), _nYields);

        if (_isNChunkSkipsSet) builder.appendNumber(nChunkSkips(), _nChunkSkips);

        if (_isMillisSet) builder.appendNumber(millis(), _millis);

        if (_isIndexBoundsSet) builder.append(indexBounds(), _indexBounds);

        if (_allPlans.get()) {
            BSONArrayBuilder allPlansBuilder(builder.subarrayStart(allPlans()));
            for (std::vector<TypeExplain*>::const_iterator it = _allPlans->begin();
                 it != _allPlans->end();
                 ++it) {
                BSONObj allPlansObject = (*it)->toBSON();
                allPlansBuilder.append(allPlansObject);
            }
            allPlansBuilder.done();
        }

        if (_oldPlan.get()) builder.append(oldPlan(), _oldPlan->toBSON());

        if (_isServerSet) builder.append(server(), _server);

        if (_isIndexFilterAppliedSet) builder.append(indexFilterApplied(), _indexFilterApplied);

        // Add this at the end as it can be huge
        if (!stats.isEmpty()) {
            builder.append("stats", stats);
        }

        return builder.obj();
    }

    bool TypeExplain::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;

        std::vector<TypeExplain*>* bareClauses = NULL;
        fieldState = FieldParser::extract(source, clauses, &bareClauses, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        if (fieldState == FieldParser::FIELD_SET) _clauses.reset(bareClauses);

        fieldState = FieldParser::extract(source, cursor, &_cursor, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isCursorSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, isMultiKey, &_isMultiKey, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isIsMultiKeySet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, n, &_n, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, nScannedObjects, &_nScannedObjects, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNScannedObjectsSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, nScanned, &_nScanned, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNScannedSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source,
                                          nScannedObjectsAllPlans,
                                          &_nScannedObjectsAllPlans,
                                          errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNScannedObjectsAllPlansSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, nScannedAllPlans, &_nScannedAllPlans, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNScannedAllPlansSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, scanAndOrder, &_scanAndOrder, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isScanAndOrderSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, indexOnly, &_indexOnly, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isIndexOnlySet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, nYields, &_nYields, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNYieldsSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, nChunkSkips, &_nChunkSkips, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNChunkSkipsSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, millis, &_millis, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isMillisSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, indexBounds, &_indexBounds, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isIndexBoundsSet = fieldState == FieldParser::FIELD_SET;

        std::vector<TypeExplain*>* bareAllPlans = NULL;
        fieldState = FieldParser::extract(source, allPlans, &bareAllPlans, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        if (fieldState == FieldParser::FIELD_SET) _allPlans.reset(bareAllPlans);

        TypeExplain* bareOldPlan = NULL;
        fieldState = FieldParser::extract(source, oldPlan, &bareOldPlan, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        if (fieldState == FieldParser::FIELD_SET) _oldPlan.reset(bareOldPlan);

        fieldState = FieldParser::extract(source, server, &_server, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isServerSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void TypeExplain::clear() {
        unsetClauses();

        _cursor.clear();
        _isCursorSet = false;

        _isMultiKey = false;
        _isIsMultiKeySet = false;

        _n = 0;
        _isNSet = false;

        _nScannedObjects = 0;
        _isNScannedObjectsSet = false;

        _nScanned = 0;
        _isNScannedSet = false;

        _nScannedObjectsAllPlans = 0;
        _isNScannedObjectsAllPlansSet = false;

        _nScannedAllPlans = 0;
        _isNScannedAllPlansSet = false;

        _scanAndOrder = false;
        _isScanAndOrderSet = false;

        _indexOnly = false;
        _isIndexOnlySet = false;

        _idHack = false;
        _isIDHackSet = false;

        _indexFilterApplied = false;
        _isIndexFilterAppliedSet = false;

        _nYields = 0;
        _isNYieldsSet = false;

        _nChunkSkips = 0;
        _isNChunkSkipsSet = false;

        _millis = 0;
        _isMillisSet = false;

        _indexBounds = BSONObj();
        _isIndexBoundsSet = false;

        unsetAllPlans();

        unsetOldPlan();

        _server.clear();
        _isServerSet = false;

    }

    void TypeExplain::cloneTo(TypeExplain* other) const {
        other->clear();

        other->unsetClauses();
        if (_clauses.get()) {
            for(std::vector<TypeExplain*>::const_iterator it = _clauses->begin();
                it != _clauses->end();
                ++it) {
                TypeExplain* clausesItem = new TypeExplain;
                (*it)->cloneTo(clausesItem);
                other->addToClauses(clausesItem);
            }
        }

        other->_cursor = _cursor;
        other->_isCursorSet = _isCursorSet;

        other->_isMultiKey = _isMultiKey;
        other->_isIsMultiKeySet = _isIsMultiKeySet;

        other->_n = _n;
        other->_isNSet = _isNSet;

        other->_nScannedObjects = _nScannedObjects;
        other->_isNScannedObjectsSet = _isNScannedObjectsSet;

        other->_nScanned = _nScanned;
        other->_isNScannedSet = _isNScannedSet;

        other->_nScannedObjectsAllPlans = _nScannedObjectsAllPlans;
        other->_isNScannedObjectsAllPlansSet = _isNScannedObjectsAllPlansSet;

        other->_nScannedAllPlans = _nScannedAllPlans;
        other->_isNScannedAllPlansSet = _isNScannedAllPlansSet;

        other->_scanAndOrder = _scanAndOrder;
        other->_isScanAndOrderSet = _isScanAndOrderSet;

        other->_indexOnly = _indexOnly;
        other->_isIndexOnlySet = _isIndexOnlySet;

        other->_idHack = _idHack;
        other->_isIDHackSet = _isIDHackSet;

        other->_indexFilterApplied = _indexFilterApplied;
        other->_isIndexFilterAppliedSet = _isIndexFilterAppliedSet;

        other->_nYields = _nYields;
        other->_isNYieldsSet = _isNYieldsSet;

        other->_nChunkSkips = _nChunkSkips;
        other->_isNChunkSkipsSet = _isNChunkSkipsSet;

        other->_millis = _millis;
        other->_isMillisSet = _isMillisSet;

        other->_indexBounds = _indexBounds;
        other->_isIndexBoundsSet = _isIndexBoundsSet;

        other->unsetAllPlans();
        if (_allPlans.get()) {
            for(std::vector<TypeExplain*>::const_iterator it = _allPlans->begin();
                it != _allPlans->end();
                ++it) {
                TypeExplain* allPlansItem = new TypeExplain;
                (*it)->cloneTo(allPlansItem);
                other->addToAllPlans(allPlansItem);
            }
        }

        other->unsetOldPlan();
        if (_oldPlan.get()) {
            TypeExplain* oldPlanCopy = new TypeExplain;
            _oldPlan->cloneTo(oldPlanCopy);
            other->setOldPlan(oldPlanCopy);
        }

        other->_server = _server;
        other->_isServerSet = _isServerSet;
    }

    std::string TypeExplain::toString() const {
        return toBSON().toString();
    }

    void TypeExplain::setClauses(const std::vector<TypeExplain*>& clauses) {
        unsetClauses();
        for(std::vector<TypeExplain*>::const_iterator it = clauses.begin();
            it != clauses.end();
            ++it) {
            TypeExplain* clausesItem = new TypeExplain;
            (*it)->cloneTo(clausesItem);
            addToClauses(clausesItem);
        }
    }

    void TypeExplain::addToClauses(TypeExplain* clauses) {
        if (_clauses.get() == NULL) {
            _clauses.reset(new std::vector<TypeExplain*>);
        }
        _clauses->push_back(clauses);
    }

    void TypeExplain::unsetClauses() {
        if (_clauses.get()) {
            for(std::vector<TypeExplain*>::const_iterator it = _clauses->begin();
                it != _clauses->end();
                ++it) {
                delete *it;
            }
        }
        _clauses.reset();
    }

    bool TypeExplain::isClausesSet() const {
        return _clauses.get() != NULL;
    }

    size_t TypeExplain::sizeClauses() const {
        verify(_clauses.get());
        return _clauses->size();
    }

    const std::vector<TypeExplain*>& TypeExplain::getClauses() const {
        verify(_clauses.get());
        return *_clauses;
    }

    const TypeExplain* TypeExplain::getClausesAt(size_t pos) const {
        verify(_clauses.get());
        verify(_clauses->size() > pos);
        return _clauses->at(pos);
    }

    void TypeExplain::setCursor(const StringData& cursor) {
        _cursor = cursor.toString();
        _isCursorSet = true;
    }

    void TypeExplain::unsetCursor() {
         _isCursorSet = false;
     }

    bool TypeExplain::isCursorSet() const {
         return _isCursorSet;
    }

    const std::string& TypeExplain::getCursor() const {
        verify(_isCursorSet);
        return _cursor;
    }

    void TypeExplain::setIsMultiKey(bool isMultiKey) {
        _isMultiKey = isMultiKey;
        _isIsMultiKeySet = true;
    }

    void TypeExplain::unsetIsMultiKey() {
         _isIsMultiKeySet = false;
     }

    bool TypeExplain::isIsMultiKeySet() const {
         return _isIsMultiKeySet;
    }

    bool TypeExplain::getIsMultiKey() const {
        verify(_isIsMultiKeySet);
        return _isMultiKey;
    }

    void TypeExplain::setN(long long n) {
        _n = n;
        _isNSet = true;
    }

    void TypeExplain::unsetN() {
         _isNSet = false;
     }

    bool TypeExplain::isNSet() const {
         return _isNSet;
    }

    long long TypeExplain::getN() const {
        verify(_isNSet);
        return _n;
    }

    void TypeExplain::setNScannedObjects(long long nScannedObjects) {
        _nScannedObjects = nScannedObjects;
        _isNScannedObjectsSet = true;
    }

    void TypeExplain::unsetNScannedObjects() {
         _isNScannedObjectsSet = false;
     }

    bool TypeExplain::isNScannedObjectsSet() const {
         return _isNScannedObjectsSet;
    }

    long long TypeExplain::getNScannedObjects() const {
        verify(_isNScannedObjectsSet);
        return _nScannedObjects;
    }

    void TypeExplain::setNScanned(long long nScanned) {
        _nScanned = nScanned;
        _isNScannedSet = true;
    }

    void TypeExplain::unsetNScanned() {
         _isNScannedSet = false;
     }

    bool TypeExplain::isNScannedSet() const {
         return _isNScannedSet;
    }

    long long TypeExplain::getNScanned() const {
        verify(_isNScannedSet);
        return _nScanned;
    }

    void TypeExplain::setNScannedObjectsAllPlans(long long nScannedObjectsAllPlans) {
        _nScannedObjectsAllPlans = nScannedObjectsAllPlans;
        _isNScannedObjectsAllPlansSet = true;
    }

    void TypeExplain::unsetNScannedObjectsAllPlans() {
         _isNScannedObjectsAllPlansSet = false;
     }

    bool TypeExplain::isNScannedObjectsAllPlansSet() const {
         return _isNScannedObjectsAllPlansSet;
    }

    long long TypeExplain::getNScannedObjectsAllPlans() const {
        verify(_isNScannedObjectsAllPlansSet);
        return _nScannedObjectsAllPlans;
    }

    void TypeExplain::setNScannedAllPlans(long long nScannedAllPlans) {
        _nScannedAllPlans = nScannedAllPlans;
        _isNScannedAllPlansSet = true;
    }

    void TypeExplain::unsetNScannedAllPlans() {
         _isNScannedAllPlansSet = false;
     }

    bool TypeExplain::isNScannedAllPlansSet() const {
         return _isNScannedAllPlansSet;
    }

    long long TypeExplain::getNScannedAllPlans() const {
        verify(_isNScannedAllPlansSet);
        return _nScannedAllPlans;
    }

    void TypeExplain::setScanAndOrder(bool scanAndOrder) {
        _scanAndOrder = scanAndOrder;
        _isScanAndOrderSet = true;
    }

    void TypeExplain::unsetScanAndOrder() {
         _isScanAndOrderSet = false;
     }

    bool TypeExplain::isScanAndOrderSet() const {
         return _isScanAndOrderSet;
    }

    bool TypeExplain::getScanAndOrder() const {
        verify(_isScanAndOrderSet);
        return _scanAndOrder;
    }

    void TypeExplain::setIndexOnly(bool indexOnly) {
        _indexOnly = indexOnly;
        _isIndexOnlySet = true;
    }

    void TypeExplain::unsetIndexOnly() {
         _isIndexOnlySet = false;
     }

    bool TypeExplain::isIndexOnlySet() const {
         return _isIndexOnlySet;
    }

    bool TypeExplain::getIndexOnly() const {
        verify(_isIndexOnlySet);
        return _indexOnly;
    }

    void TypeExplain::setIDHack(bool idhack) {
        _idHack = idhack;
        _isIDHackSet = true;
    }

    void TypeExplain::unsetIDHack() {
        _isIDHackSet = false;
    }

    bool TypeExplain::isIDHackSet() const {
        return _isIDHackSet;
    }

    bool TypeExplain::getIDHack() const {
        verify(_isIDHackSet);
        return _idHack;
    }

    void TypeExplain::setIndexFilterApplied(bool indexFilterApplied) {
        _indexFilterApplied = indexFilterApplied;
        _isIndexFilterAppliedSet = true;
    }

    void TypeExplain::unsetIndexFilterApplied() {
        _isIndexFilterAppliedSet = false;
    }

    bool TypeExplain::isIndexFilterAppliedSet() const {
        return _isIndexFilterAppliedSet;
    }

    bool TypeExplain::getIndexFilterApplied() const {
        verify(_isIndexFilterAppliedSet);
        return _indexFilterApplied;
    }

    void TypeExplain::setNYields(long long nYields) {
        _nYields = nYields;
        _isNYieldsSet = true;
    }

    void TypeExplain::unsetNYields() {
         _isNYieldsSet = false;
     }

    bool TypeExplain::isNYieldsSet() const {
         return _isNYieldsSet;
    }

    long long TypeExplain::getNYields() const {
        verify(_isNYieldsSet);
        return _nYields;
    }

    void TypeExplain::setNChunkSkips(long long nChunkSkips) {
        _nChunkSkips = nChunkSkips;
        _isNChunkSkipsSet = true;
    }

    void TypeExplain::unsetNChunkSkips() {
         _isNChunkSkipsSet = false;
     }

    bool TypeExplain::isNChunkSkipsSet() const {
         return _isNChunkSkipsSet;
    }

    long long TypeExplain::getNChunkSkips() const {
        verify(_isNChunkSkipsSet);
        return _nChunkSkips;
    }

    void TypeExplain::setMillis(long long millis) {
        _millis = millis;
        _isMillisSet = true;
    }

    void TypeExplain::unsetMillis() {
         _isMillisSet = false;
     }

    bool TypeExplain::isMillisSet() const {
         return _isMillisSet;
    }

    long long TypeExplain::getMillis() const {
        verify(_isMillisSet);
        return _millis;
    }

    void TypeExplain::setIndexBounds(const BSONObj& indexBounds) {
        _indexBounds = indexBounds.getOwned();
        _isIndexBoundsSet = true;
    }

    void TypeExplain::unsetIndexBounds() {
         _isIndexBoundsSet = false;
     }

    bool TypeExplain::isIndexBoundsSet() const {
         return _isIndexBoundsSet;
    }

    const BSONObj& TypeExplain::getIndexBounds() const {
        verify(_isIndexBoundsSet);
        return _indexBounds;
    }

    void TypeExplain::setAllPlans(const std::vector<TypeExplain*>& allPlans) {
        unsetAllPlans();
        for (std::vector<TypeExplain*>::const_iterator it = allPlans.begin();
             it != allPlans.end();
             ++it) {
            TypeExplain* allPlansItem = new TypeExplain;
            (*it)->cloneTo(allPlansItem);
            addToClauses(allPlansItem);
        }
    }

    void TypeExplain::addToAllPlans(TypeExplain* allPlans) {
        if (_allPlans.get() == NULL) {
            _allPlans.reset(new std::vector<TypeExplain*>);
        }
        _allPlans->push_back(allPlans);
    }

    void TypeExplain::unsetAllPlans() {
        if (_allPlans.get()) {
            for (std::vector<TypeExplain*>::const_iterator it = _allPlans->begin();
             it != _allPlans->end();
             ++it) {
                delete *it;
            }
            _allPlans.reset();
        }
    }

    bool TypeExplain::isAllPlansSet() const {
        return _allPlans.get() != NULL;
    }

    size_t TypeExplain::sizeAllPlans() const {
        verify(_allPlans.get());
        return _allPlans->size();
    }

    const std::vector<TypeExplain*>& TypeExplain::getAllPlans() const {
        verify(_allPlans.get());
        return *_allPlans;
    }

    const TypeExplain* TypeExplain::getAllPlansAt(size_t pos) const {
        verify(_allPlans.get());
        verify(_allPlans->size() > pos);
        return _allPlans->at(pos);
    }

    void TypeExplain::setOldPlan(TypeExplain* oldPlan) {
        _oldPlan.reset(oldPlan);
    }

    void TypeExplain::unsetOldPlan() {
        _oldPlan.reset();
     }

    bool TypeExplain::isOldPlanSet() const {
        return _oldPlan.get() != NULL;
    }

    const TypeExplain* TypeExplain::getOldPlan() const {
        verify(_oldPlan.get());
        return _oldPlan.get();
    }

    void TypeExplain::setServer(const StringData& server) {
        _server = server.toString();
        _isServerSet = true;
    }

    void TypeExplain::unsetServer() {
         _isServerSet = false;
     }

    bool TypeExplain::isServerSet() const {
         return _isServerSet;
    }

    const std::string& TypeExplain::getServer() const {
        verify(_isServerSet);
        return _server;
    }

} // namespace mongo
