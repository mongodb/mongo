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

#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/fle/query_rewriter.h"

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
                const EncryptionInformation& encryptInfo,
                bool allowEmptySchema);

    boost::intrusive_ptr<ExpressionContext> expCtx;
    const NamespaceString nssEsc;
    const std::map<NamespaceString, NamespaceString>
        _escMap;  // Map collection Ns to ESC metadata collection nss.
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
                  EncryptedCollScanModeAllowed mode);

    ~FilterRewrite() override {};

    void doRewrite(FLETagQueryInterface* queryImpl) final;

    const BSONObj userFilter;
    BSONObj rewrittenFilter;
    EncryptedCollScanModeAllowed _mode;
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
};
}  // namespace fle
}  // namespace mongo
