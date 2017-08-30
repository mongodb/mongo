/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/db/pipeline/document_source_change_stream.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/bson/bson_helper.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/pipeline/close_change_stream_exception.h"
#include "mongo/db/pipeline/document_source_check_resume_token.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using std::string;
using std::vector;

// The $changeStream stage is an alias for many stages, but we need to be able to serialize
// and re-parse the pipeline. To make this work, the 'transformation' stage will serialize itself
// with the original specification, and all other stages that are created during the alias expansion
// will not serialize themselves.
REGISTER_MULTI_STAGE_ALIAS(changeStream,
                           DocumentSourceChangeStream::LiteParsed::parse,
                           DocumentSourceChangeStream::createFromBson);

constexpr StringData DocumentSourceChangeStream::kDocumentKeyField;
constexpr StringData DocumentSourceChangeStream::kFullDocumentField;
constexpr StringData DocumentSourceChangeStream::kIdField;
constexpr StringData DocumentSourceChangeStream::kNamespaceField;
constexpr StringData DocumentSourceChangeStream::kUuidField;
constexpr StringData DocumentSourceChangeStream::kOperationTypeField;
constexpr StringData DocumentSourceChangeStream::kStageName;
constexpr StringData DocumentSourceChangeStream::kTimestampField;
constexpr StringData DocumentSourceChangeStream::kClusterTimeField;
constexpr StringData DocumentSourceChangeStream::kUpdateOpType;
constexpr StringData DocumentSourceChangeStream::kDeleteOpType;
constexpr StringData DocumentSourceChangeStream::kReplaceOpType;
constexpr StringData DocumentSourceChangeStream::kInsertOpType;
constexpr StringData DocumentSourceChangeStream::kInvalidateOpType;

namespace {

static constexpr StringData kOplogMatchExplainName = "$_internalOplogMatch"_sd;

}  // namespace

intrusive_ptr<DocumentSourceOplogMatch> DocumentSourceOplogMatch::create(
    BSONObj filter, const intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceOplogMatch(std::move(filter), expCtx);
}

const char* DocumentSourceOplogMatch::getSourceName() const {
    // This is used in error reporting, particularly if we find this stage in a position other
    // than first, so report the name as $changeStream.
    return DocumentSourceChangeStream::kStageName.rawData();
}

DocumentSource::StageConstraints DocumentSourceOplogMatch::constraints() const {
    StageConstraints constraints;
    constraints.requiredPosition = PositionRequirement::kFirst;
    constraints.isAllowedInsideFacetStage = false;
    return constraints;
}

/**
 * Only serialize this stage for explain purposes, otherwise keep it hidden so that we can
 * properly alias.
 */
Value DocumentSourceOplogMatch::serialize(optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        return Value(Document{{kOplogMatchExplainName, Document{}}});
    }
    return Value();
}

DocumentSourceOplogMatch::DocumentSourceOplogMatch(BSONObj filter,
                                                   const intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceMatch(std::move(filter), expCtx) {}

void checkValueType(const Value v, const StringData filedName, BSONType expectedType) {
    uassert(40532,
            str::stream() << "Entry field \"" << filedName << "\" should be "
                          << typeName(expectedType)
                          << ", found: "
                          << typeName(v.getType()),
            (v.getType() == expectedType));
}

namespace {
/**
 * This stage is used internally for change notifications to close cursor after returning
 * "invalidate" entries.
 * It is not intended to be created by the user.
 */
class DocumentSourceCloseCursor final : public DocumentSource {
public:
    GetNextResult getNext() final;

    const char* getSourceName() const final {
        // This is used in error reporting.
        return "$changeStream";
    }

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        // This stage is created by the DocumentSourceChangeStream stage, so serializing it
        // here would result in it being created twice.
        return Value();
    }

    static boost::intrusive_ptr<DocumentSourceCloseCursor> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceCloseCursor(expCtx);
    }

private:
    /**
     * Use the create static method to create a DocumentSourceCloseCursor.
     */
    DocumentSourceCloseCursor(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(expCtx) {}

    bool _shouldCloseCursor = false;
};

DocumentSource::GetNextResult DocumentSourceCloseCursor::getNext() {
    pExpCtx->checkForInterrupt();

    // Close cursor if we have returned an invalidate entry.
    if (_shouldCloseCursor) {
        throw CloseChangeStreamException();
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced())
        return nextInput;

    auto doc = nextInput.getDocument();
    const auto& kOperationTypeField = DocumentSourceChangeStream::kOperationTypeField;
    checkValueType(doc[kOperationTypeField], kOperationTypeField, BSONType::String);
    if (doc[kOperationTypeField].getString() == DocumentSourceChangeStream::kInvalidateOpType) {
        // Pass the invalidation forward, so that it can be included in the results, or
        // filtered/transformed by further stages in the pipeline, then throw an exception
        // to close the cursor on the next call to getNext().
        _shouldCloseCursor = true;
    }

    return nextInput;
}

}  // namespace

BSONObj DocumentSourceChangeStream::buildMatchFilter(const NamespaceString& nss,
                                                     Timestamp startFrom,
                                                     bool isResume) {
    auto target = nss.ns();

    // 1) Supported commands that have the target db namespace (e.g. test.$cmd) in "ns" field.
    auto dropDatabase = BSON("o.dropDatabase" << 1);
    auto dropCollection = BSON("o.drop" << nss.coll());
    auto renameCollection = BSON("o.renameCollection" << target);
    // 1.1) Commands that are on target db and one of the above.
    auto commandsOnTargetDb =
        BSON("ns" << nss.getCommandNS().ns() << OR(dropDatabase, dropCollection, renameCollection));
    // 1.2) Supported commands that have arbitrary db namespaces in "ns" field.
    auto renameDropTarget = BSON("o.to" << target);
    // All supported commands that are either (1.1) or (1.2).
    BSONObj commandMatch = BSON("op"
                                << "c"
                                << OR(commandsOnTargetDb, renameDropTarget));

    // 2) Normal CRUD ops on the target collection.
    auto opMatch = BSON("ns" << target);

    // Match oplog entries after "start" and are either (1) supported commands or (2) CRUD ops,
    // excepting those tagged "fromMigrate".
    // Include the resume token, if resuming, so we can verify it was still present in the oplog.
    return BSON("$and" << BSON_ARRAY(BSON("ts" << (isResume ? GTE : GT) << startFrom)
                                     << BSON(OR(opMatch, commandMatch))
                                     << BSON("fromMigrate" << NE << true)));
}

list<intrusive_ptr<DocumentSource>> DocumentSourceChangeStream::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    // TODO: Add sharding support here (SERVER-29141).
    uassert(
        40470, "The $changeStream stage is not supported on sharded systems.", !expCtx->inRouter);
    uassert(40471,
            "Only default collation is allowed when using a $changeStream stage.",
            !expCtx->getCollator());

    auto replCoord = repl::ReplicationCoordinator::get(expCtx->opCtx);
    uassert(40573, "The $changeStream stage is only supported on replica sets", replCoord);
    Timestamp startFrom = replCoord->getMyLastAppliedOpTime().getTimestamp();

    intrusive_ptr<DocumentSource> resumeStage = nullptr;
    auto spec = DocumentSourceChangeStreamSpec::parse(IDLParserErrorContext("$changeStream"),
                                                      elem.embeddedObject());
    if (auto resumeAfter = spec.getResumeAfter()) {
        ResumeToken token = resumeAfter.get();
        auto resumeNamespace = UUIDCatalog::get(expCtx->opCtx).lookupNSSByUUID(token.getUuid());
        uassert(40615,
                "The resume token UUID does not exist. Has the collection been dropped?",
                !resumeNamespace.isEmpty());
        startFrom = token.getTimestamp();
        if (expCtx->needsMerge) {
            DocumentSourceShardCheckResumabilitySpec spec;
            spec.setResumeToken(std::move(token));
            resumeStage = DocumentSourceShardCheckResumability::create(expCtx, std::move(spec));
        } else {
            DocumentSourceEnsureResumeTokenPresentSpec spec;
            spec.setResumeToken(std::move(token));
            resumeStage = DocumentSourceEnsureResumeTokenPresent::create(expCtx, std::move(spec));
        }
    }
    const bool changeStreamIsResuming = resumeStage != nullptr;

    auto fullDocOption = spec.getFullDocument();
    uassert(40575,
            str::stream() << "unrecognized value for the 'fullDocument' option to the "
                             "$changeStream stage. Expected \"default\" or "
                             "\"updateLookup\", got \""
                          << fullDocOption
                          << "\"",
            fullDocOption == "updateLookup"_sd || fullDocOption == "default"_sd);
    const bool shouldLookupPostImage = (fullDocOption == "updateLookup"_sd);

    auto oplogMatch = DocumentSourceOplogMatch::create(
        buildMatchFilter(expCtx->ns, startFrom, changeStreamIsResuming), expCtx);
    auto transformation = createTransformationStage(elem.embeddedObject(), expCtx);
    list<intrusive_ptr<DocumentSource>> stages = {oplogMatch, transformation};
    if (resumeStage) {
        stages.push_back(resumeStage);
    }
    auto closeCursorSource = DocumentSourceCloseCursor::create(expCtx);
    stages.push_back(closeCursorSource);
    if (shouldLookupPostImage) {
        stages.push_back(DocumentSourceLookupChangePostImage::create(expCtx));
    }
    return stages;
}

intrusive_ptr<DocumentSource> DocumentSourceChangeStream::createTransformationStage(
    BSONObj changeStreamSpec, const intrusive_ptr<ExpressionContext>& expCtx) {
    return intrusive_ptr<DocumentSource>(new DocumentSourceSingleDocumentTransformation(
        expCtx, stdx::make_unique<Transformation>(changeStreamSpec), kStageName.toString()));
}

Document DocumentSourceChangeStream::Transformation::applyTransformation(const Document& input) {
    MutableDocument doc;

    // Extract the fields we need.
    checkValueType(input[repl::OplogEntry::kOpTypeFieldName],
                   repl::OplogEntry::kOpTypeFieldName,
                   BSONType::String);
    string op = input[repl::OplogEntry::kOpTypeFieldName].getString();
    Value ts = input[repl::OplogEntry::kTimestampFieldName];
    Value ns = input[repl::OplogEntry::kNamespaceFieldName];
    checkValueType(ns, repl::OplogEntry::kNamespaceFieldName, BSONType::String);
    Value uuid = input[repl::OplogEntry::kUuidFieldName];
    if (!uuid.missing())
        checkValueType(uuid, repl::OplogEntry::kUuidFieldName, BSONType::BinData);
    NamespaceString nss(ns.getString());
    Value id = input.getNestedField("o._id");
    // Non-replace updates have the _id in field "o2".
    Value documentId = id.missing() ? input.getNestedField("o2._id") : id;
    StringData operationType;
    Value fullDocument;
    Value updateDescription;

    // Deal with CRUD operations and commands.
    auto opType = repl::OpType_parse(IDLParserErrorContext("ChangeStreamEntry.op"), op);
    switch (opType) {
        case repl::OpTypeEnum::kInsert: {
            operationType = kInsertOpType;
            fullDocument = input[repl::OplogEntry::kObjectFieldName];
            break;
        }
        case repl::OpTypeEnum::kDelete: {
            operationType = kDeleteOpType;
            break;
        }
        case repl::OpTypeEnum::kUpdate: {
            if (id.missing()) {
                operationType = kUpdateOpType;
                checkValueType(input[repl::OplogEntry::kObjectFieldName],
                               repl::OplogEntry::kObjectFieldName,
                               BSONType::Object);
                Document opObject = input[repl::OplogEntry::kObjectFieldName].getDocument();
                Value updatedFields = opObject["$set"];
                Value removedFields = opObject["$unset"];

                // Extract the field names of $unset document.
                vector<Value> removedFieldsVector;
                if (removedFields.getType() == BSONType::Object) {
                    auto iter = removedFields.getDocument().fieldIterator();
                    while (iter.more()) {
                        removedFieldsVector.push_back(Value(iter.next().first));
                    }
                }
                updateDescription = Value(Document{
                    {"updatedFields", updatedFields.missing() ? Value(Document()) : updatedFields},
                    {"removedFields", removedFieldsVector}});
            } else {
                operationType = kReplaceOpType;
                fullDocument = input[repl::OplogEntry::kObjectFieldName];
            }
            break;
        }
        case repl::OpTypeEnum::kCommand: {
            operationType = kInvalidateOpType;
            // Make sure the result doesn't have a document id.
            documentId = Value();
            break;
        }
        default: { MONGO_UNREACHABLE; }
    }

    // UUID should always be present except for invalidate entries.
    invariant(operationType == kInvalidateOpType || !uuid.missing());

    // Construct the result document.
    Value documentKey;
    if (!documentId.missing()) {
        documentKey = Value(Document{{kIdField, documentId}});
    }
    // Note that 'documentKey' might be missing, in which case it will not appear in the output.
    Document resumeToken{{kClusterTimeField, Document{{kTimestampField, ts}}},
                         {kUuidField, uuid},
                         {kDocumentKeyField, documentKey}};
    doc.addField(kIdField, Value(resumeToken));
    doc.addField(kOperationTypeField, Value(operationType));
    doc.addField(kFullDocumentField, fullDocument);

    // "invalidate" entry has fewer fields.
    if (opType == repl::OpTypeEnum::kCommand) {
        return doc.freeze();
    }

    doc.addField(kNamespaceField, Value(Document{{"db", nss.db()}, {"coll", nss.coll()}}));
    doc.addField(kDocumentKeyField, documentKey);

    // Note that 'updateDescription' might be the 'missing' value, in which case it will not be
    // serialized.
    doc.addField("updateDescription", updateDescription);
    return doc.freeze();
}

Document DocumentSourceChangeStream::Transformation::serializeStageOptions(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Document(_changeStreamSpec);
}

DocumentSource::GetDepsReturn DocumentSourceChangeStream::Transformation::addDependencies(
    DepsTracker* deps) const {
    deps->fields.insert(repl::OplogEntry::kOpTypeFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kTimestampFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kNamespaceFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kUuidFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kObjectFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kObject2FieldName.toString());
    return DocumentSource::GetDepsReturn::EXHAUSTIVE_ALL;
}

DocumentSource::GetModPathsReturn DocumentSourceChangeStream::Transformation::getModifiedPaths()
    const {
    // All paths are modified.
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, std::set<string>{}, {}};
}

}  // namespace mongo
