// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// The parts of IncrementalFeatureRolloutContext that operate on an OperationContext. They live
// here (in the 'service_context' library), rather than in feature_flag.cpp (in the 'server_base'
// library) because 'server_base' is also linked into small tools that carry no
// Client/OperationContext machinery. Code that uses those types can require symbols from
// 'service_context' (sanitizer builds, for example), which would break linking for any binary that
// uses 'server_base' alone. See version_context_decoration.cpp for the same split.

#include "mongo/db/client.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/util/deferred.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

namespace mongo {
namespace {

// Use a lazy-initialization here to avoid frontloading any work for traffic which doesn't consult
// any feature flags.
using DeferredIfrContext = Deferred<std::shared_ptr<IncrementalFeatureRolloutContext> (*)()>;
struct OpCtxIfrContext {
    DeferredIfrContext deferred{[] {
        return std::make_shared<IncrementalFeatureRolloutContext>();
    }};
};
const auto getIfrContextOnOpCtx = OperationContext::declareDecoration<OpCtxIfrContext>();

}  // namespace

// static
std::shared_ptr<IncrementalFeatureRolloutContext> IncrementalFeatureRolloutContext::get(
    OperationContext* opCtx) {
    auto& deferred = getIfrContextOnOpCtx(opCtx).deferred;
    // A nested DBDirectClient op (e.g. the auth user-cache lookup) can run on the parent's opCtx
    // before the parent installs its wire IFR context -- the install happens in the
    // InvocationBaseInternal ctor, so anything running earlier (auth/pre-parse) precedes it,
    // whereas command-body work like view resolution always runs after it. The context installs
    // once, so materializing it here with local defaults would make the parent's wire install a
    // no-op and silently drop a value the router set -- e.g. a flag it disabled after a kickback.
    // Hand nested ops a detached context instead; once the parent installs, nested ops inherit it.
    if (!deferred.isInitialized() && opCtx->getClient()->isInDirectClient()) {
        return std::make_shared<IncrementalFeatureRolloutContext>();
    }
    // Forces materialization of whatever initializer is present (the default empty context, or one
    // an install path assigned).
    return *deferred;
}

// static
std::shared_ptr<IncrementalFeatureRolloutContext> IncrementalFeatureRolloutContext::tryGet(
    OperationContext* opCtx) {
    // Never forces; an unmaterialized context is treated as absent.
    auto& deferred = getIfrContextOnOpCtx(opCtx).deferred;
    return deferred.isInitialized() ? *deferred : nullptr;
}

// static
bool IncrementalFeatureRolloutContext::isInstalled(OperationContext* opCtx) {
    return getIfrContextOnOpCtx(opCtx).deferred.isInitialized();
}

// static
void IncrementalFeatureRolloutContext::set(OperationContext* opCtx,
                                           std::shared_ptr<IncrementalFeatureRolloutContext> ctx) {
    auto& deferred = getIfrContextOnOpCtx(opCtx).deferred;
    // Install-once invariant: replacing a context that already cached its egress serialization
    // would silently drop that resolved state. Only a materialized context can have cached it.
    if (deferred.isInitialized()) {
        tassert(13002310,
                "Refusing to replace an IFRContext whose egress metadata has already been cached",
                !(*deferred)->hasCachedEgressMetadataForTest());
    }
    // Eager install: value is materialized immediately, so it is visible to tryGet()/isInstalled()
    // and serialized on egress.
    deferred = DeferredIfrContext(std::move(ctx));
}

// static
void IncrementalFeatureRolloutContext::installForRequestWithoutIfrFlags(OperationContext* opCtx) {

    // A shard server that receives no ifrFlags got the request from a pre-9.0 router, an old shard,
    // or a background thread — none of which coordinated a value — should conservatively disable
    // every release flag introduced at kLatest to keep shards from turning a feature on before
    // their siblings do. Any shard might be the first upgraded in the deployment, so we need to
    // wait for a signal that other shards are ready to turn on a new behavior across the whole
    // cluster. This will come when the routing layer is upgraded.
    //
    // This is deliberately FCV-independent, and in balance with a pure replica set deployment. In
    // a replica set deployment we expect requests from clients to come in soon after upgrading
    // (before setFCV command) to use the new defaults of the feature flags. So in this case the
    // lack of ifrFlags on the request indicates we should use whatever the current settings are on
    // this node.
    //
    // We discriminate between the two based on whether we have the shard server role.
    //
    // This isn't great, but it's a stopgap. In the v9.1+ future, a "pre-9.0 router" won't exist,
    // and we will expect all incoming requests to have their flags specified. Then we can remove
    // this condition and have no fear using the local settings if we don't hear otherwise.
    const auto clusterRole = serverGlobalParams.clusterRole;
    if (clusterRole.has(ClusterRole::ShardServer)) {
        // Defer cloning the all-flags-off template until a flag is actually consulted. If nothing
        // consults one, the context stays unmaterialized and no ifrFlags are forwarded downstream.
        getIfrContextOnOpCtx(opCtx).deferred =
            DeferredIfrContext([] { return mutableShardServerDefaultTemplate().clone(); });
    } else if (clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        // Avoid deferring initialization on mongos - we want to eagerly initialize to make sure
        // everyone's on the same page.
        set(opCtx, std::make_shared<IncrementalFeatureRolloutContext>());
    }
}

}  // namespace mongo
