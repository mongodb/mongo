// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/fle/query_rewriter.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
class FLETagQueryInterface;

namespace fle {

/**
 * RewriteBase is responsible for performing the server rewrites for FLE2. RewriteBase is the
 * interface used in server_rewrite::doFLERewriteInTxn(), and relies on the key virtual method
 * doRewrite() to perform the implementation specific rewrite work. The implementation of
 * RewriteBase and all its derived classes reside in server_rewrite.cpp, as the implementation
 * details are dependent on internals of server_rewrite.cpp. Historically, these classes were
 * declared in server_rewrite.cpp, but were moved into their own header to facilitate unit
 * testing.
 */
class RewriteBase {
public:
    virtual ~RewriteBase() {};

    virtual void doRewrite(FLETagQueryInterface* queryImpl) = 0;

protected:
    RewriteBase(boost::intrusive_ptr<ExpressionContext> expCtx,
                const NamespaceString& nss,
                const NamespaceString& escNss,
                const EncryptionInformation& encryptInfo);

    boost::intrusive_ptr<ExpressionContext> expCtx;
    const NamespaceString nssEsc;
    // Map collection Ns to its EncryptedFieldConfig.
    const std::map<NamespaceString, EncryptedFieldConfig> _efcMap;
};

/**
 * FilterRewrite is responsible for rewriting a single match expression (represented as a BSONObj).
 * This class is used to perform the server rewrites for commands that have a "query" filter, such
 * as Find, Count, Delete and Update.
 */
class FilterRewrite : public RewriteBase {
public:
    FilterRewrite(boost::intrusive_ptr<ExpressionContext> expCtx,
                  const NamespaceString& nss,
                  const EncryptionInformation& encryptInfo,
                  BSONObj toRewrite,
                  EncryptedCollScanModeAllowed mode,
                  const EncryptedFieldConfig& validatedConfig);

    ~FilterRewrite() override {};

    void doRewrite(FLETagQueryInterface* queryImpl) final;

    const BSONObj userFilter;
    BSONObj rewrittenFilter;
    EncryptedCollScanModeAllowed _mode;
    const EncryptedFieldConfig _efc;
};

/**
 * PipelineRewrite is responsible for rewriting an entire aggregation pipeline. The implementation
 * of doRewrite() relies on a map of DocumentSource type to a rewrite functor (see stageRewriterMap
 * in server_rewrite.cpp) in order to perform the rewrite of each stage within the pipeline.
 */
class PipelineRewrite : public RewriteBase {
public:
    PipelineRewrite(const NamespaceString& nss,
                    const EncryptionInformation& encryptInfo,
                    std::unique_ptr<Pipeline> toRewrite);
    ~PipelineRewrite() override {};

    void doRewrite(FLETagQueryInterface* queryImpl) final;

    std::unique_ptr<Pipeline> getPipeline();

protected:
    // This method is used specifically for unit testing, allowing the unit tests to provide their
    // own mocked QueryRewriter.
    virtual QueryRewriter getQueryRewriterForEsc(FLETagQueryInterface* queryImpl);

    std::unique_ptr<Pipeline> _pipeline;
    // Unset for an unencrypted collection with encrypted $lookup sub-pipelines.
    const boost::optional<EncryptedFieldConfig> _efc;
};
}  // namespace fle
}  // namespace mongo
