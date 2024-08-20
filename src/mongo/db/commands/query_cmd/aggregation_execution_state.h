/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/external_data_source_scope_guard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/util/deferred.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/sharding_state.h"
#include "mongo/util/string_map.h"
#include <boost/optional.hpp>

namespace mongo {
/*
 * We use a DeferredFn<BSONObj> to attempt to optimize aggregation on views. When aggregating on a
 * view, we create a new request using the resolved view and call back into the aggregation
 * execution code. The new request (based on the resolved view) warrants a new
 * AggregateCommandRequest, but we don't actually need to materialize a BSONObj version of it unless
 * we need to register a cursor (i.e if results do not fit in a single batch). In some cases,
 * serializing the command object can consume a significant amount of time that we can optimize away
 * if the results fit in a single batch.
 */
using DeferredCmd = DeferredFn<BSONObj>;

/**
 * AggExState (short for AggregationExecutionState) is a class specifically designed to aid
 * in the execution of the top-level aggregate command execution code on a mongod.

 * The execution of an aggregate command is complex with many steps and branches, most/all of which
 * share some common context. This class factors out the most commonly used referenced pieces of
 * state that are central to the execution of the aggregation.
 *
 * This class also holds some member functions that soley, or nearly, rely on state held within the
 * class, that also assists in the execution of the aggregation.
 */
class AggExState {
public:
    /*
     * Upon construction, the context always references the inital request it is constructed with.
     * However, if upon computation it is determined that aggregation is on a view, the 'setView()'
     * function can be used to reset the state to a new equivelant aggregation pipeline on the
     * underlying collection.
     */
    AggExState(OperationContext* opCtx,
               AggregateCommandRequest& request,
               const LiteParsedPipeline& liteParsedPipeline,
               const BSONObj& cmdObj,
               const PrivilegeVector& privileges,
               const std::vector<std::pair<NamespaceString, std::vector<ExternalDataSourceInfo>>>&
                   usedExternalDataSources)
        : _aggReqDerivatives(new AggregateRequestDerivatives(request, liteParsedPipeline, cmdObj)),
          _opCtx(opCtx),
          _executionNss(request.getNamespace()),
          _privileges(privileges),
          _originalAggReqDerivatives(request, liteParsedPipeline, cmdObj) {
        // Create virtual collections and drop them when aggregate command is done.
        // If a cursor is registered, the ExternalDataSourceScopeGuard will be stored in the cursor;
        // when the cursor is later destroyed, the scope guard will also be destroyed, and any
        // virtual collections will be dropped by the destructor of ExternalDataSourceScopeGuard. We
        // create this scope guard prior to taking locks in _runAggregate so that, if no cursor is
        // registered, the virtual collections will be dropped after releasing our read locks,
        // avoiding a lock upgrade.
        _externalDataSourceGuard = usedExternalDataSources.size() > 0
            ? std::make_shared<ExternalDataSourceScopeGuard>(_opCtx, usedExternalDataSources)
            : nullptr;
    }

    /* Getter functions. Please note that some values can change after setting the view or
     * namespace. */

    OperationContext* getOpCtx() const {
        return _opCtx;
    }

    AggregateCommandRequest& getRequest() const {
        return _aggReqDerivatives->request;
    }

    // Returns the original request the class was constructed with,
    // regardless if setView() has been called.
    const AggregateCommandRequest& getOriginalRequest() const {
        return _originalAggReqDerivatives.request;
    }

    const NamespaceString& getExecutionNss() const {
        return _executionNss;
    }

    // Returns the namespace of the original request the class was constructed with,
    // regardless if setView() has been called.
    const NamespaceString& getOriginalNss() const {
        return _originalAggReqDerivatives.request.getNamespace();
    }

    const DeferredCmd& getDeferredCmd() const {
        return _aggReqDerivatives->cmdObj;
    }

    const PrivilegeVector& getPrivileges() const {
        return _privileges;
    }

    std::shared_ptr<ExternalDataSourceScopeGuard> getExternalDataSourceScopeGuard() const {
        return _externalDataSourceGuard;
    }

    // Returns all the namespaces that the aggregation involves.
    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const {
        return _aggReqDerivatives->liteParsedPipeline.getInvolvedNamespaces();
    }

    // Returns all the namespaces involved in the aggregation that are not the originating
    // namespaces / collection.
    // i.e. all namespaces referenced in $lookup/$unionWith stages.
    std::vector<NamespaceStringOrUUID> getForeignExecutionNamespaces() const {
        return _aggReqDerivatives->liteParsedPipeline.getForeignExecutionNamespaces();
    }

    boost::optional<const ResolvedView&> getResolvedView() const {
        if (_resolvedView.has_value()) {
            return boost::optional<const ResolvedView&>(_resolvedView.value());
        } else {
            return boost::none;
        }
    }

    StatusWith<StringMap<ExpressionContext::ResolvedNamespace>> resolveInvolvedNamespaces() const;

    /* Setter functions */

    // Explicitly set the namespace of the aggregation,
    // not derived from the request or resolved view (notably the oplog)
    void setExecutionNss(NamespaceString nss) {
        _executionNss = nss;
    }

    // Once the aggregation is calculated to be on view, this function is used to set
    // the internal state of the aggregation execution on the resolved view.
    // TODO SERVER-93539 remove this function and construct a ResolvedViewAggExState instead.
    void setView(std::shared_ptr<const CollectionCatalog> catalog, const ViewDefinition* view);

    // Only to be used after this class has been set as a resolved view.
    // TODO SERVER-93539 put this function only on the ResolvedViewAggExState.
    ScopedSetShardRole setShardRole(const CollectionRoutingInfo& cri);

    /* Checking member functions (returns bool / status describing the aggregation state) */

    bool aggIsOnView() const {
        return _resolvedView.has_value();
    }

    // True iff aggregation has a $changeStream stage.
    bool hasChangeStream() const {
        return _aggReqDerivatives->liteParsedPipeline.hasChangeStream();
    }

    // True iff aggregation starts with a $collStats stage.
    bool startsWithCollStats() const {
        return _aggReqDerivatives->liteParsedPipeline.startsWithCollStats();
    }

    bool canReadUnderlyingCollectionLocally(const CollectionRoutingInfo& cri) const;

    // Returns Status::OK if each view namespace in 'pipeline' has a default collator equivalent
    // to 'collator'. Otherwise, returns ErrorCodes::OptionNotSupportedOnView.
    Status collatorCompatibleWithPipeline(const CollatorInterface* collator) const;

    /* Misc member functions */

    // Performs validations related to API versioning, time-series stages, and general command
    // validation.
    // Throws UserAssertion if any of the validations fails
    //     - validation of API versioning on each stage on the pipeline
    //     - validation of API versioning on 'AggregateCommandRequest' request
    //     - validation of time-series related stages
    //     - validation of command parameters
    void performValidationChecks();

    // Upconverts the read concern for a change stream aggregation, if necesssary.
    //
    // If there is no given read concern level on the given object, upgrades the level to 'majority'
    // and waits for read concern. If a read concern level is already specified on the given read
    // concern object, this method does nothing.
    void adjustChangeStreamReadConcern();

    // Increments global stage counters corresponding to the stages in this lite parsed pipeline.
    void tickGlobalStageCounters() const {
        _aggReqDerivatives->liteParsedPipeline.tickGlobalStageCounters();
    }

private:
    /**
     * This inner struct holds the AggregateCommandRequest, as well derivative structures that
     * are are processed from it (LiteParsedPipeline & DeferredCmd).
     * The processing of these other structures takes a non-trivial
     * amount of time, so they are passed into AggregationExecutionState by reference
     * (from previous use) and we want to avoid reconstructing them internally.
     * TODO SERVER-93536 make these member variables by value by move assignment,
     * instead of reference.
     */
    class AggregateRequestDerivatives {
    public:
        AggregateCommandRequest& request;
        // Request derivatives
        const LiteParsedPipeline& liteParsedPipeline;
        const DeferredCmd cmdObj;

        // Constructor for when the BSONObj is already constructed.
        AggregateRequestDerivatives(AggregateCommandRequest& request,
                                    const LiteParsedPipeline& liteParsedPipeline,
                                    const BSONObj& cmdObj)
            : request(request),
              liteParsedPipeline(liteParsedPipeline),
              cmdObj(DeferredCmd(cmdObj)) {}

        // Constructor for when the BSONObj's construction is to be deferred
        AggregateRequestDerivatives(AggregateCommandRequest& request,
                                    const LiteParsedPipeline& liteParsedPipeline,
                                    std::function<BSONObj()> BSONObjFn)
            : request(request), liteParsedPipeline(liteParsedPipeline), cmdObj(BSONObjFn) {}
    };

    // The AggregateRequestDerivatives variables is wrapped in a unique_ptr because it needs to
    // be swapped out for another instance in the case that the aggregation is on a view. This
    // object cannot be reassigned directly because it has reference member variables.
    // This pointer should never be null; the indirection only exists to support re-assignment.
    // TODO SERVER-93536 remove pointer wrapper once internal variables of AggregateRequestDerivates
    // are values instead of references.
    std::unique_ptr<AggregateRequestDerivatives> _aggReqDerivatives;

    // Set upon construction and never reset. Should never be nullptr.
    OperationContext* _opCtx;

    // This is the namespace that the aggregation will be executed in.
    // This is sometimes, but not necessarily the same namespace as original request.
    // Alternatively, the execution namespace can be set to the namespace of the
    // collection underlying a view, or the oplog for changestreams.
    // This object is stored as a value instead of a reference, because its possible to set
    // this value with an rvalue (essentially construct a new one internally), instead of getting
    // an external reference. Also, its cheap to copy this object because it is small.
    NamespaceString _executionNss;

    // 'privileges' contains the privileges that were required to run this aggregation, to be used
    // later for re-checking privileges for getMore commands.
    const PrivilegeVector& _privileges;

    std::shared_ptr<ExternalDataSourceScopeGuard> _externalDataSourceGuard;

    /* The following member variables are added to support views */

    // An 'original' copy of the request derivatives struct is kept for the case where the
    // aggregation is on a view. To process an aggregation on a view the underlying collection
    // and pipeline are first resolved, and then the aggregation is re-processed as if it were
    // on a the resolved collection with an updated request that is equivalent to the original
    // request. Both the primary and original copies of this struct should be set upon construction.
    // We should not be worried about memory costs as the underlying struct only holds
    // references. This variable will never be reassigned after construction.
    AggregateRequestDerivatives _originalAggReqDerivatives;

    // Once '_aggReqDerivatives' points to a new object related to the resolved view parameters,
    // the member references point to these underlying class variables.
    // Unfortunately we can't change the underlying member fields in AggregateCommandRequest
    // to not be references because we accept these variables as such in the constructor.
    // This class would need a way to have complete ownership of the currently referenced variables
    // (perhaps with move semantics) to get rid of these member variables.
    boost::optional<AggregateCommandRequest> _resolvedViewRequest;
    boost::optional<LiteParsedPipeline> _resolvedViewLiteParsedPipeline;

    // Has a value iff the aggregation is on a view. It will always start without a value,
    // and then optionally be populated upon calculation that the aggregation is on a view.
    // Is used both as a demarcator of if the aggregation is on a view,
    // and for the underlying value itself.
    boost::optional<ResolvedView> _resolvedView;
};
}  // namespace mongo
