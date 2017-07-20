/**
 *    Copyright (C) 2013 10gen Inc.
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

namespace mongo {

namespace str = mongoutils::str;
namespace mb = mongo::mutablebson;

using std::unique_ptr;
using std::vector;

using pathsupport::EqualityMatches;

namespace {

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
StatusWith<bool> parseUpdateExpression(
    BSONObj updateExpr,
    UpdateObjectNode* root,
    const CollatorInterface* collator,
    const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters) {
    bool positional = false;
    std::set<std::string> foundIdentifiers;
    for (auto&& mod : updateExpr) {
        auto modType = validateMod(mod);
        for (auto&& field : mod.Obj()) {
            auto statusWithPositional = UpdateObjectNode::parseAndMerge(
                root, modType, field, collator, arrayFilters, foundIdentifiers);
            if (!statusWithPositional.isOK()) {

                // Check whether we failed to parse because this mod type is not yet supported by
                // UpdateNode.
                // TODO SERVER-28777: Skip this check.
                auto leaf = modifiertable::makeUpdateLeafNode(modType);
                if (!leaf) {
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream() << "Cannot use array filters with modifier "
                                          << mod.fieldName(),
                            arrayFilters.empty());
                    return statusWithPositional;
                }

                uassertStatusOK(statusWithPositional);
                MONGO_UNREACHABLE;
            }
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

UpdateDriver::UpdateDriver(const Options& opts)
    : _replacementMode(false),
      _indexedFields(NULL),
      _logOp(opts.logOp),
      _modOptions(opts.modOptions),
      _affectIndices(false),
      _positional(false) {}

UpdateDriver::~UpdateDriver() {
    clear();
}

Status UpdateDriver::parse(
    const BSONObj& updateExpr,
    const std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>& arrayFilters,
    const bool multi) {
    clear();

    // Check if the update expression is a full object replacement.
    if (*updateExpr.firstElementFieldName() != '$') {
        if (multi) {
            return Status(ErrorCodes::FailedToParse, "multi update only works with $ operators");
        }

        _root = stdx::make_unique<ObjectReplaceNode>(updateExpr);

        // Register the fact that this driver will only do full object replacements.
        _replacementMode = true;

        return Status::OK();
    }

    // Register the fact that this driver is not doing a full object replacement.
    _replacementMode = false;

    // If the featureCompatibilityVersion is 3.6, parse using UpdateNode.
    // TODO SERVER-28777: Remove the restriction that this is only done if the update is not from
    // replication.
    if (serverGlobalParams.featureCompatibility.version.load() ==
            ServerGlobalParams::FeatureCompatibility::Version::k36 &&
        !_modOptions.fromReplication) {
        auto root = stdx::make_unique<UpdateObjectNode>();
        auto statusWithPositional =
            parseUpdateExpression(updateExpr, root.get(), _modOptions.collator, arrayFilters);
        if (statusWithPositional.isOK()) {
            _positional = statusWithPositional.getValue();
            _root = std::move(root);
            return Status::OK();
        }
    }

    // TODO SERVER-28777: This can be an else case, since we will not fall back to the old parsing
    // if we fail to parse using the new implementation.
    uassert(ErrorCodes::InvalidOptions,
            "The featureCompatibilityVersion must be 3.6 to use arrayFilters. See "
            "http://dochub.mongodb.org/core/3.6-feature-compatibility.",
            arrayFilters.empty());
    for (auto&& mod : updateExpr) {
        auto modType = validateMod(mod);
        for (auto&& field : mod.Obj()) {
            auto status = addAndParse(modType, field);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    return Status::OK();
}

inline Status UpdateDriver::addAndParse(const modifiertable::ModifierType type,
                                        const BSONElement& elem) {
    if (elem.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "'" << elem.fieldName() << "' has no value in : " << elem
                                    << " which is not allowed for any $"
                                    << type
                                    << " mod.");
    }

    unique_ptr<ModifierInterface> mod(modifiertable::makeUpdateMod(type));
    dassert(mod.get());

    bool positional = false;
    Status status = mod->init(elem, _modOptions, &positional);
    if (!status.isOK()) {
        return status;
    }

    // If any modifier indicates that it requires a positional match, toggle the
    // _positional flag to true.
    _positional = _positional || positional;

    _mods.push_back(mod.release());

    return Status::OK();
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
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx, std::move(qr), ExtensionsCallbackNoop());
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
                            BSONObj original,
                            mutablebson::Document* doc,
                            bool validateForStorage,
                            const FieldRefSet& immutablePaths,
                            BSONObj* logOpRec,
                            bool* docWasModified) {
    // TODO: assert that update() is called at most once in a !_multi case.

    _affectIndices = (isDocReplacement() && (_indexedFields != NULL));

    _logDoc.reset();
    LogBuilder logBuilder(_logDoc.root());

    if (_root) {

        // We parsed using the new UpdateNode implementation.
        FieldRef pathToCreate;
        FieldRef pathTaken;
        bool indexesAffected = false;
        bool noop = false;
        _root->apply(doc->root(),
                     &pathToCreate,
                     &pathTaken,
                     matchedField,
                     _modOptions.fromReplication,
                     validateForStorage,
                     immutablePaths,
                     _indexedFields,
                     (_logOp && logOpRec) ? &logBuilder : nullptr,
                     &indexesAffected,
                     &noop);
        if (indexesAffected) {
            _affectIndices = true;
            doc->disableInPlaceUpdates();
        }
        if (docWasModified) {
            *docWasModified = !noop;
        }

    } else {

        // We parsed using the old ModifierInterface implementation.
        // Ask each of the mods to type check whether they can operate over the current document
        // and, if so, to change that document accordingly.
        FieldRefSet updatedPaths;

        for (vector<ModifierInterface*>::iterator it = _mods.begin(); it != _mods.end(); ++it) {
            ModifierInterface::ExecInfo execInfo;
            Status status = (*it)->prepare(doc->root(), matchedField, &execInfo);
            if (!status.isOK()) {
                return status;
            }

            // If a mod wants to be applied only if this is an upsert (or only if this is a
            // strict update), we should respect that. If a mod doesn't care, it would state
            // it is fine with ANY update context.
            const bool validContext =
                (execInfo.context == ModifierInterface::ExecInfo::ANY_CONTEXT ||
                 execInfo.context == _context);

            // Nothing to do if not in a valid context.
            if (!validContext) {
                continue;
            }


            // Gather which fields this mod is interested on and whether these fields were
            // "taken" by previous mods.  Note that not all mods are multi-field mods. When we
            // see an empty field, we may stop looking for others.
            for (int i = 0; i < ModifierInterface::ExecInfo::MAX_NUM_FIELDS; i++) {
                if (execInfo.fieldRef[i] == 0) {
                    break;
                }

                // Record each field being updated but check for conflicts first
                const FieldRef* other;
                if (!updatedPaths.insert(execInfo.fieldRef[i], &other)) {
                    return Status(ErrorCodes::ConflictingUpdateOperators,
                                  str::stream() << "Cannot update '" << other->dottedField()
                                                << "' and '"
                                                << execInfo.fieldRef[i]->dottedField()
                                                << "' at the same time");
                }

                // We start with the expectation that a mod will be in-place. But if the mod
                // touched an indexed field and the mod will indeed be executed -- that is, it
                // is not a no-op and it is in a valid context -- then we switch back to a
                // non-in-place mode.
                //
                // TODO: make mightBeIndexed and fieldRef like each other.
                if (!_affectIndices && !execInfo.noOp && _indexedFields &&
                    _indexedFields->mightBeIndexed(execInfo.fieldRef[i]->dottedField())) {
                    _affectIndices = true;
                    doc->disableInPlaceUpdates();
                }
            }

            if (!execInfo.noOp) {
                status = (*it)->apply();

                if (docWasModified)
                    *docWasModified = true;

                if (!status.isOK()) {
                    return status;
                }
            }

            // If we require a replication oplog entry for this update, go ahead and generate one.
            if (!execInfo.noOp && _logOp && logOpRec) {
                status = (*it)->log(&logBuilder);
                if (!status.isOK()) {
                    return status;
                }
            }
        }

        // Check for BSON depth and DBRef constraint violations.
        if (validateForStorage) {
            for (auto path = updatedPaths.begin(); path != updatedPaths.end(); ++path) {

                // Find the updated field in the updated document.
                auto newElem = doc->root();
                for (size_t i = 0; i < (*path)->numParts(); ++i) {
                    newElem = newElem[(*path)->getPart(i)];
                    if (!newElem.ok()) {
                        break;
                    }
                }

                // newElem might be missing if $unset/$renamed-away.
                if (newElem.ok()) {

                    // Check parents.
                    const std::uint32_t recursionLevel = 0;
                    auto parentsDepth =
                        storage_validation::storageValidParents(newElem, recursionLevel);

                    // Check element and its children.
                    const bool doRecursiveCheck = true;
                    storage_validation::storageValid(newElem, doRecursiveCheck, parentsDepth);
                }
            }
        }

        for (auto path = immutablePaths.begin(); path != immutablePaths.end(); ++path) {

            if (!updatedPaths.findConflicts(*path, nullptr)) {
                continue;
            }

            // Find the updated field in the updated document.
            auto newElem = doc->root();
            for (size_t i = 0; i < (*path)->numParts(); ++i) {
                newElem = newElem[(*path)->getPart(i)];
                if (!newElem.ok()) {
                    break;
                }
                uassert(ErrorCodes::NotSingleValueField,
                        str::stream()
                            << "After applying the update to the document, the (immutable) field '"
                            << (*path)->dottedField()
                            << "' was found to be an array or array descendant.",
                        newElem.getType() != BSONType::Array);
            }

            auto oldElem =
                dotted_path_support::extractElementAtPath(original, (*path)->dottedField());

            uassert(ErrorCodes::ImmutableField,
                    str::stream() << "After applying the update, the '" << (*path)->dottedField()
                                  << "' (required and immutable) field was "
                                     "found to have been removed --"
                                  << original,
                    newElem.ok() || !oldElem.ok());
            if (newElem.ok() && oldElem.ok()) {
                uassert(ErrorCodes::ImmutableField,
                        str::stream() << "After applying the update, the (immutable) field '"
                                      << (*path)->dottedField()
                                      << "' was found to have been altered to "
                                      << newElem.toString(),
                        newElem.compareWithBSONElement(oldElem, nullptr, false) == 0);
            }
        }
    }

    if (_logOp && logOpRec)
        *logOpRec = _logDoc.getObject();

    return Status::OK();
}

size_t UpdateDriver::numMods() const {
    return _mods.size();
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

ModifierInterface::Options UpdateDriver::modOptions() const {
    return _modOptions;
}

void UpdateDriver::setModOptions(ModifierInterface::Options modOpts) {
    _modOptions = modOpts;
}

void UpdateDriver::setCollator(const CollatorInterface* collator) {
    if (_root) {
        _root->setCollator(collator);
    }

    for (auto&& mod : _mods) {
        mod->setCollator(collator);
    }

    _modOptions.collator = collator;
}

ModifierInterface::ExecInfo::UpdateContext UpdateDriver::context() const {
    return _context;
}

void UpdateDriver::setContext(ModifierInterface::ExecInfo::UpdateContext context) {
    _context = context;
}

BSONObj UpdateDriver::makeOplogEntryQuery(const BSONObj& doc, bool multi) const {
    BSONObjBuilder idPattern;
    BSONElement id;
    // NOTE: If the matching object lacks an id, we'll log
    // with the original pattern.  This isn't replay-safe.
    // It might make sense to suppress the log instead
    // if there's no id.
    if (doc.getObjectID(id)) {
        idPattern.append(id);
        return idPattern.obj();
    } else {
        uassert(16980,
                str::stream() << "Multi-update operations require all documents to "
                                 "have an '_id' field. "
                              << doc.toString(),
                !multi);
        return doc;
    }
}
void UpdateDriver::clear() {
    for (vector<ModifierInterface*>::iterator it = _mods.begin(); it != _mods.end(); ++it) {
        delete *it;
    }
    _mods.clear();
    _indexedFields = NULL;
    _replacementMode = false;
    _positional = false;
}

}  // namespace mongo
