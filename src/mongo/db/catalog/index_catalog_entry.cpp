/**
*    Copyright (C) 2017 10gen Inc.
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

#include "mongo/db/catalog/index_catalog_entry.h"

#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {
IndexCatalogEntry::Impl::~Impl() = default;

namespace {
stdx::function<IndexCatalogEntry::factory_function_type> factory;
}  // namespace

void IndexCatalogEntry::registerFactory(decltype(factory) newFactory) {
    factory = std::move(newFactory);
}

auto IndexCatalogEntry::makeImpl(IndexCatalogEntry* const this_,
                                 OperationContext* const opCtx,
                                 const StringData ns,
                                 CollectionCatalogEntry* const collection,
                                 std::unique_ptr<IndexDescriptor> descriptor,
                                 CollectionInfoCache* const infoCache) -> std::unique_ptr<Impl> {
    return factory(this_, opCtx, ns, collection, std::move(descriptor), infoCache);
}

void IndexCatalogEntry::TUHook::hook() noexcept {}

IndexCatalogEntry::IndexCatalogEntry(OperationContext* opCtx,
                                     StringData ns,
                                     CollectionCatalogEntry* collection,
                                     std::unique_ptr<IndexDescriptor> descriptor,
                                     CollectionInfoCache* infoCache)
    : _pimpl(makeImpl(this, opCtx, ns, collection, std::move(descriptor), infoCache)) {}

void IndexCatalogEntry::init(std::unique_ptr<IndexAccessMethod> accessMethod) {
    return this->_impl().init(std::move(accessMethod));
}

// ------------------

const IndexCatalogEntry* IndexCatalogEntryContainer::find(const IndexDescriptor* desc) const {
    if (desc->_cachedEntry)
        return desc->_cachedEntry;

    for (const_iterator i = begin(); i != end(); ++i) {
        const IndexCatalogEntry* e = i->get();
        if (e->descriptor() == desc)
            return e;
    }
    return nullptr;
}

IndexCatalogEntry* IndexCatalogEntryContainer::find(const IndexDescriptor* desc) {
    if (desc->_cachedEntry)
        return desc->_cachedEntry;

    for (iterator i = begin(); i != end(); ++i) {
        IndexCatalogEntry* e = i->get();
        if (e->descriptor() == desc)
            return e;
    }
    return nullptr;
}

IndexCatalogEntry* IndexCatalogEntryContainer::find(const std::string& name) {
    for (iterator i = begin(); i != end(); ++i) {
        IndexCatalogEntry* e = i->get();
        if (e->descriptor()->indexName() == name)
            return e;
    }
    return nullptr;
}

IndexCatalogEntry* IndexCatalogEntryContainer::release(const IndexDescriptor* desc) {
    for (auto i = _entries.begin(); i != _entries.end(); ++i) {
        if ((*i)->descriptor() != desc)
            continue;
        IndexCatalogEntry* e = i->release();
        _entries.erase(i);
        return e;
    }
    return nullptr;
}
}  // namespace mongo
