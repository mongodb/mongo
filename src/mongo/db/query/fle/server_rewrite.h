// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

/**
 * This file contains the interface for rewriting filters within CRUD commands for FLE2.
 */
namespace [[MONGO_MOD_PUBLIC]] mongo {
class FLETagQueryInterface;

namespace fle {


/**
 * Make a collator object from its BSON representation. Useful when creating ExpressionContext
 * objects for parsing MatchExpressions as part of the server-side rewrite.
 */
std::unique_ptr<CollatorInterface> collatorFromBSON(OperationContext* opCtx,
                                                    const BSONObj& collation);

/**
 * Return a rewritten version of the passed-in filter as a BSONObj. Generally used by other
 * functions to process MatchExpressions in each command.
 */
BSONObj rewriteQuery(OperationContext* opCtx,
                     boost::intrusive_ptr<ExpressionContext> expCtx,
                     const NamespaceString& nss,
                     const EncryptionInformation& info,
                     BSONObj filter,
                     GetTxnCallback getTransaction,
                     EncryptedCollScanModeAllowed mode,
                     const EncryptedFieldConfig& validatedEfc);

/**
 * Process a find command with encryptionInformation in-place, rewriting the filter condition so
 * that any query on an encrypted field will properly query the underlying tags array.
 */
void processFindCommand(OperationContext* opCtx,
                        const NamespaceString& nss,
                        FindCommandRequest* findCommand,
                        GetTxnCallback txn);

/**
 * Process a count command with encryptionInformation in-place, rewriting the filter condition so
 * that any query on an encrypted field will properly query the underlying tags array.
 */
void processCountCommand(OperationContext* opCtx,
                         const NamespaceString& nss,
                         CountCommandRequest* countCommand,
                         GetTxnCallback getTxn);

/**
 * Process a pipeline with encryptionInformation by rewriting the pipeline to query against the
 * underlying tags array, where appropriate. After this rewriting is complete, there is no more
 * FLE work to be done. The encryption info does not need to be kept around (e.g. on a command
 * object).
 */
std::unique_ptr<Pipeline> processPipeline(OperationContext* opCtx,
                                          NamespaceString nss,
                                          const EncryptionInformation& encryptInfo,
                                          std::unique_ptr<Pipeline> toRewrite,
                                          GetTxnCallback txn);

/**
 * Rewrite a filter MatchExpression with FLE Find Payloads into a disjunction over the tag array
 * from inside an existing transaction using a FLETagQueryInterface constructed from a
 * transaction client.
 */
BSONObj rewriteEncryptedFilterInsideTxn(
    FLETagQueryInterface* queryImpl,
    const NamespaceString& nss,
    const EncryptedFieldConfig& efc,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    BSONObj filter,
    EncryptedCollScanModeAllowed mode = EncryptedCollScanModeAllowed::kDisallow);
}  // namespace fle
}  // namespace mongo
