/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/index/index_descriptor.h"


namespace mongo {
IndexCatalog::Impl::~Impl() = default;

namespace {
IndexCatalog::factory_function_type factory;
}  // namespace

void IndexCatalog::registerFactory(decltype(factory) newFactory) {
    factory = std::move(newFactory);
}

auto IndexCatalog::makeImpl(IndexCatalog* const this_,
                            Collection* const collection,
                            const int maxNumIndexesAllowed) -> std::unique_ptr<Impl> {
    return factory(this_, collection, maxNumIndexesAllowed);
}

void IndexCatalog::TUHook::hook() noexcept {}

IndexCatalogEntry* IndexCatalog::_setupInMemoryStructures(
    OperationContext* const opCtx,
    std::unique_ptr<IndexDescriptor> descriptor,
    const bool initFromDisk) {
    return this->_impl()._setupInMemoryStructures(opCtx, std::move(descriptor), initFromDisk);
}


IndexCatalog::IndexIterator::Impl::~Impl() = default;

namespace {
IndexCatalog::IndexIterator::factory_function_type iteratorFactory;
}  // namespace

void IndexCatalog::IndexIterator::registerFactory(decltype(iteratorFactory) newFactory) {
    iteratorFactory = std::move(newFactory);
}

auto IndexCatalog::IndexIterator::makeImpl(OperationContext* const opCtx,
                                           const IndexCatalog* const cat,
                                           const bool includeUnfinishedIndexes)
    -> std::unique_ptr<Impl> {
    return iteratorFactory(opCtx, cat, includeUnfinishedIndexes);
}

void IndexCatalog::IndexIterator::TUHook::hook() noexcept {}

namespace {
stdx::function<decltype(IndexCatalog::fixIndexKey)> fixIndexKeyImpl;
}  // namespace

void IndexCatalog::registerFixIndexKeyImpl(decltype(fixIndexKeyImpl) impl) {
    fixIndexKeyImpl = std::move(impl);
}

BSONObj IndexCatalog::fixIndexKey(const BSONObj& key) {
    return fixIndexKeyImpl(key);
}

namespace {
stdx::function<decltype(IndexCatalog::prepareInsertDeleteOptions)> prepareInsertDeleteOptionsImpl;
}  // namespace

void IndexCatalog::prepareInsertDeleteOptions(OperationContext* const opCtx,
                                              const IndexDescriptor* const desc,
                                              InsertDeleteOptions* const options) {
    return prepareInsertDeleteOptionsImpl(opCtx, desc, options);
}

void IndexCatalog::registerPrepareInsertDeleteOptionsImpl(
    stdx::function<decltype(prepareInsertDeleteOptions)> impl) {
    prepareInsertDeleteOptionsImpl = std::move(impl);
}
}  // namespace mongo
