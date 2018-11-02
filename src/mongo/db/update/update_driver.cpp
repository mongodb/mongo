
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

#include "mongo/db/update/update_driver.h"


#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/server_options.h"
#include "mongo/db/update/log_builder.h"
#include "mongo/db/update/modifier_table.h"
#include "mongo/db/update/object_replace_node.h"
#include "mongo/db/update/path_support.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/util/embedded_builder.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {

namespace str = mongoutils::str;
namespace mb = mongo::mutablebson;

using std::unique_ptr;
using std::vector;

using pathsupport::EqualityMatches;

namespace {

StatusWith<UpdateSemantics> updateSemanticsFromElement(BSONElement element) {
    if (element.type() != BSONType::NumberInt && element.type() != BSONType::NumberLong) {
        return {ErrorCodes::BadValue, "'$v' (UpdateSemantics) field must be an integer."};
    }

    auto updateSemantics = element.numberLong();

    // As of 3.7, we only support one version of the update language.
    if (updateSemantics != static_cast<int>(UpdateSemantics::kUpdateNode)) {
        return {ErrorCodes::Error(40682),
                str::stream() << "Unrecognized value for '$v' (UpdateSemantics) field: "
                              << updateSemantics};
    }

    return static_cast<UpdateSemantics>(updateSemantics);
}

modifiertable::ModifierType validateMod(BSONElement mod) {
    auto modType = modifiertable::getType(mod.fieldName());

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Unknown modifier: " << mod.fieldName(),
            modType != modifiertable::MOD_UNKNOWN);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Modifiers operate on fields but we found type "
                          << typeName(mod.type())
                          << " instead. For example: {$mod: {<field>: ...}}"
                          << " not {"
                          << mod
                          << "}",
            mod.type() == BSONType::Object);

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "'" << mod.fieldName()
                          << "' is empty. You must specify a field like so: "
                             "{"
                          << mod.fieldName()
                          << ": {<field>: ...}}",
            !mod.embeddedObject().isEmpty());

    return modType;
}

// Parses 'updateExpr' and merges it into 'root'. Returns whether 'updateExpr' is positional.
bool parseUpdateExpression(
    BSONObj updateExpr,
    UpdateObjectNode* root,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters) {
    bool positional = false;
    std::set<std::string> foundIdentifiers;
    bool foundUpdateSemanticsField = false;
    for (auto&& mod : updateExpr) {
        // If there is a "$v" field among the modifiers, it should have already been used by the
        // caller to determine that this is the correct parsing function.
        if (mod.fieldNameStringData() == LogBuilder::kUpdateSemanticsFieldName) {
            uassert(ErrorCodes::BadValue,
                    "Duplicate $v in oplog update document",
                    !foundUpdateSemanticsField);
            foundUpdateSemanticsField = true;
            invariant(mod.numberLong() == static_cast<long long>(UpdateSemantics::kUpdateNode));
            continue;
        }

        auto modType = validateMod(mod);
        for (auto&& field : mod.Obj()) {
            auto statusWithPositional = UpdateObjectNode::parseAndMerge(
                root, modType, field, expCtx, arrayFilters, foundIdentifiers);
            uassertStatusOK(statusWithPositional);
            positional = positional || statusWithPositional.getValue();
        }
    }

    for (const auto& arrayFilter : arrayFilters) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "The array filter for identifier '" << arrayFilter.first
                              << "' was not used in the update "
                              << updateExpr,
                foundIdentifiers.find(arrayFilter.first.toString()) != foundIdentifiers.end());
    }

    return positional;
}

}  // namespace

UpdateDriver::UpdateDriver(const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : _expCtx(expCtx) {}

void UpdateDriver::parse(
    const BSONObj& updateExpr,
    const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters,
    const bool multi) {
    invariant(!_root && !_replacementMode, "Multiple calls to parse() on same UpdateDriver");

    // Check if the update expression is a full object replacement.
    if (isDocReplacement(updateExpr)) {
        uassert(ErrorCodes::FailedToParse, "multi update only works with $ operators", !multi);

        _root = stdx::make_unique<ObjectReplaceNode>(updateExpr);

        // Register the fact that this driver will only do full object replacements.
        _replacementMode = true;

        return;
    }

    // Register the fact that this driver is not doing a full object replacement.
    _replacementMode = false;

    // Some versions of mongod support more than one version of the update language and look for a
    // $v "UpdateSemantics" field when applying an oplog entry, in order to know which version of
    // the update language to apply with. We currently only support the 'kUpdateNode' version, but
    // we parse $v and check its value for compatibility.
    BSONElement updateSemanticsElement = updateExpr[LogBuilder::kUpdateSemanticsFieldName];
    if (updateSemanticsElement) {
        uassert(ErrorCodes::FailedToParse,
                "The $v update field is only recognized internally",
                _fromOplogApplication);

        uassertStatusOK(updateSemanticsFromElement(updateSemanticsElement));
    }

    auto root = stdx::make_unique<UpdateObjectNode>();
    _positional = parseUpdateExpression(updateExpr, root.get(), _expCtx, arrayFilters);
    _root = std::move(root);
}

Status UpdateDriver::populateDocumentWithQueryFields(OperationContext* opCtx,
                                                     const BSONObj& query,
                                                     const FieldRefSet& immutablePaths,
                                                     mutablebson::Document& doc) const {
    // We canonicalize the query to collapse $and/$or, and the namespace is not needed.  Also,
    // because this is for the upsert case, where we insert a new document if one was not found, the
    // $where/$text clauses do not make sense, hence empty ExtensionsCallback.
    auto qr = stdx::make_unique<QueryRequest>(NamespaceString(""));
    qr->setFilter(query);
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    // $expr is not allowed in the query for an upsert, since it is not clear what the equality
    // extraction behavior for $expr should be.
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(qr),
                                     expCtx,
                                     ExtensionsCallbackNoop(),
                                     MatchExpressionParser::kAllowAllSpecialFeatures &
                                         ~MatchExpressionParser::AllowedFeatures::kExpr);
    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    return populateDocumentWithQueryFields(*cq, immutablePaths, doc);
}

Status UpdateDriver::populateDocumentWithQueryFields(const CanonicalQuery& query,
                                                     const FieldRefSet& immutablePaths,
                                                     mutablebson::Document& doc) const {
    EqualityMatches equalities;
    Status status = Status::OK();

    if (isDocReplacement()) {

        // Extract only immutable fields from replacement-style
        status =
            pathsupport::extractFullEqualityMatches(*query.root(), immutablePaths, &equalities);
    } else {
        // Extract all fields from op-style
        status = pathsupport::extractEqualityMatches(*query.root(), &equalities);
    }

    if (!status.isOK())
        return status;

    status = pathsupport::addEqualitiesToDoc(equalities, &doc);
    return status;
}

Status UpdateDriver::update(StringData matchedField,
                            mutablebson::Document* doc,
                            bool validateForStorage,
                            const FieldRefSet& immutablePaths,
                            BSONObj* logOpRec,
                            bool* docWasModified,
                            FieldRefSetWithStorage* modifiedPaths) {
    // TODO: assert that update() is called at most once in a !_multi case.

    _affectIndices = (isDocReplacement() && (_indexedFields != NULL));

    _logDoc.reset();
    LogBuilder logBuilder(_logDoc.root());

    UpdateNode::ApplyParams applyParams(doc->root(), immutablePaths);
    applyParams.matchedField = matchedField;
    applyParams.insert = _insert;
    applyParams.fromOplogApplication = _fromOplogApplication;
    applyParams.validateForStorage = validateForStorage;
    applyParams.indexData = _indexedFields;
    applyParams.modifiedPaths = modifiedPaths;
    // The supplied 'modifiedPaths' must be an empty set.
    invariant(!modifiedPaths || modifiedPaths->empty());

    if (_logOp && logOpRec) {
        applyParams.logBuilder = &logBuilder;
    }
    auto applyResult = _root->apply(applyParams);
    if (applyResult.indexesAffected) {
        _affectIndices = true;
        doc->disableInPlaceUpdates();
    }
    if (docWasModified) {
        *docWasModified = !applyResult.noop;
    }
    if (!_replacementMode && _logOp && logOpRec) {
        // If there are binVersion=3.6 mongod nodes in the replica set, they need to be told that
        // this update is using the "kUpdateNode" version of the update semantics and not the older
        // update semantics that could be used by a featureCompatibilityVersion=3.4 node.
        //
        // TODO (SERVER-32240): Once binVersion <= 3.6 nodes are not supported in a replica set, we
        // can safely elide this "$v" UpdateSemantics field from oplog entries, because there will
        // only one supported version, which all nodes will assume is in use.
        //
        // We also don't need to specify the semantics for a full document replacement (and there
        // would be no place to put a "$v" field in the update document).
        invariant(logBuilder.setUpdateSemantics(UpdateSemantics::kUpdateNode));
    }

    if (_logOp && logOpRec)
        *logOpRec = _logDoc.getObject();

    return Status::OK();
}

bool UpdateDriver::isDocReplacement() const {
    return _replacementMode;
}

bool UpdateDriver::modsAffectIndices() const {
    return _affectIndices;
}

void UpdateDriver::refreshIndexKeys(const UpdateIndexData* indexedFields) {
    _indexedFields = indexedFields;
}

bool UpdateDriver::logOp() const {
    return _logOp;
}

void UpdateDriver::setLogOp(bool logOp) {
    _logOp = logOp;
}

bool UpdateDriver::fromOplogApplication() const {
    return _fromOplogApplication;
}

void UpdateDriver::setFromOplogApplication(bool fromOplogApplication) {
    _fromOplogApplication = fromOplogApplication;
}

void UpdateDriver::setCollator(const CollatorInterface* collator) {
    if (_root) {
        _root->setCollator(collator);
    }

    _expCtx->setCollator(collator);
}

bool UpdateDriver::isDocReplacement(const BSONObj& updateExpr) {
    return *updateExpr.firstElementFieldName() != '$';
}

}  // namespace mongo
