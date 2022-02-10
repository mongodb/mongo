/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/query_planner_params.h"

namespace mongo {

/**
 * Class which holds a set of pointers to multiple collections. This class distinguishes between
 * a 'main collection' and 'secondary collections'. While the former represents the collection a
 * given command is run against, the latter represents other collections that the execution
 * engine may need to be made aware of.
 */
class MultiCollection final {
public:
    MultiCollection() = default;

    MultiCollection(boost::optional<AutoGetCollectionForReadCommandMaybeLockFree>& mainCollCtx,
                    std::vector<std::unique_ptr<AutoGetCollectionForReadCommandMaybeLockFree>>&
                        secondaryCollCtxes) {
        if (mainCollCtx) {
            _mainColl = &mainCollCtx->getCollection();
        }

        for (auto& secondaryColl : secondaryCollCtxes) {
            if (*secondaryColl) {
                // Even if 'secondaryColl' doesn't exist, we still want to include it. It is the
                // responsibility of consumers of this class to verify that a collection exists
                // before accessing it.
                _secondaryColls.emplace(secondaryColl->getNss(), secondaryColl->getCollection());
            }
        }
    }

    explicit MultiCollection(const CollectionPtr* mainColl)
        : _mainColl(mainColl), _secondaryColls({}) {}

    explicit MultiCollection(const CollectionPtr& mainColl) : MultiCollection(&mainColl) {}

    bool hasMainCollection() const {
        return _mainColl->get();
    }

    const CollectionPtr& getMainCollection() const {
        return *_mainColl;
    }

    const std::map<NamespaceString, const CollectionPtr&>& getSecondaryCollections() const {
        return _secondaryColls;
    }

    const CollectionPtr& lookupCollection(const NamespaceString& nss) const {
        if (_mainColl && nss == _mainColl->get()->ns()) {
            return *_mainColl;
        } else if (auto itr = _secondaryColls.find(nss); itr != _secondaryColls.end()) {
            return itr->second;
        }
        return CollectionPtr::null;
    }

    void clear() {
        _mainColl = &CollectionPtr::null;
        _secondaryColls.clear();
    }

private:
    const CollectionPtr* _mainColl{&CollectionPtr::null};

    // Map from namespace to a corresponding CollectionPtr.
    std::map<NamespaceString, const CollectionPtr&> _secondaryColls{};
};
}  // namespace mongo
