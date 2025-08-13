/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/agg/exchange_stage.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <memory>
#include <set>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class DocumentSourceExchange final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$_internalExchange"_sd;

    /**
     * Create an Exchange consumer. 'resourceYielder' is so the exchange may temporarily yield
     * resources (such as the Session) while waiting for other threads to do
     * work. 'resourceYielder' may be nullptr if there are no resources which need to be given up
     * while waiting.
     */
    DocumentSourceExchange(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                           boost::intrusive_ptr<exec::agg::Exchange> exchange,
                           size_t consumerId,
                           const std::shared_ptr<ResourceYielder>& yielder);

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kNotAllowed,
                UnionRequirement::kNotAllowed};
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    size_t getConsumers() const {
        return _exchange->getConsumers();
    }

    auto getExchange() const {
        return _exchange;
    }


    auto getConsumerId() const {
        return _consumerId;
    }

    void addVariableRefs(std::set<Variables::Id>* refs) const final {
        // Any correlation analysis should have happened before this stage was created.
        MONGO_UNREACHABLE;
    }

private:
    friend exec::agg::StagePtr documentSourceExchangeToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    boost::intrusive_ptr<exec::agg::Exchange> _exchange;
    const size_t _consumerId;

    // While waiting for another thread to make room in its buffer, we may want to yield certain
    // resources (such as the Session). Through this interface we can do that.
    std::shared_ptr<ResourceYielder> _resourceYielder;
};

}  // namespace mongo
