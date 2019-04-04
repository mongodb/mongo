/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kSharding

#include "merizo/platform/basic.h"

#include "merizo/db/s/collection_sharding_state.h"
#include "merizo/db/service_context.h"

namespace merizo {
namespace {

class CollectionShardingStateFactoryEmbedded final : public CollectionShardingStateFactory {
public:
    CollectionShardingStateFactoryEmbedded(ServiceContext* serviceContext)
        : CollectionShardingStateFactory(serviceContext) {}

    std::unique_ptr<CollectionShardingState> make(const NamespaceString& nss) override {
        class CollectionShardingStateStandalone final : public CollectionShardingState {
        public:
            CollectionShardingStateStandalone(NamespaceString nss) : CollectionShardingState(nss) {}

        private:
            boost::optional<ScopedCollectionMetadata> _getMetadata(
                const boost::optional<merizo::LogicalTime>& atClusterTime) override {
                return boost::none;
            }
        };

        return std::make_unique<CollectionShardingStateStandalone>(nss);
    }
};

}  // namespace

ServiceContext::ConstructorActionRegisterer collectionShardingStateFactoryRegisterer{
    "CollectionShardingStateFactory",
    [](ServiceContext* service) {
        CollectionShardingStateFactory::set(
            service, std::make_unique<CollectionShardingStateFactoryEmbedded>(service));
    },
    [](ServiceContext* service) { CollectionShardingStateFactory::clear(service); }};

}  // namespace merizo
