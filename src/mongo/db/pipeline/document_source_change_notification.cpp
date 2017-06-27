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

#include "mongo/db/pipeline/document_source_change_notification.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using std::string;
using std::vector;

// The $changeNotification stage is an alias for many stages, but we need to be able to serialize
// and re-parse the pipeline. To make this work, the 'transformation' stage will serialize itself
// with the original specification, and all other stages that are created during the alias expansion
// will not serialize themselves.
REGISTER_MULTI_STAGE_ALIAS(changeNotification,
                           DocumentSourceChangeNotification::LiteParsed::parse,
                           DocumentSourceChangeNotification::createFromBson);

constexpr StringData DocumentSourceChangeNotification::kDocumentKeyField;
constexpr StringData DocumentSourceChangeNotification::kFullDocumentField;
constexpr StringData DocumentSourceChangeNotification::kIdField;
constexpr StringData DocumentSourceChangeNotification::kNamespaceField;
constexpr StringData DocumentSourceChangeNotification::kOperationTypeField;
constexpr StringData DocumentSourceChangeNotification::kStageName;
constexpr StringData DocumentSourceChangeNotification::kTimestmapField;
constexpr StringData DocumentSourceChangeNotification::kUpdateOpType;
constexpr StringData DocumentSourceChangeNotification::kDeleteOpType;
constexpr StringData DocumentSourceChangeNotification::kReplaceOpType;
constexpr StringData DocumentSourceChangeNotification::kInsertOpType;
constexpr StringData DocumentSourceChangeNotification::kInvalidateOpType;

namespace {

static constexpr StringData kOplogMatchExplainName = "$_internalOplogMatch"_sd;

/**
 * A custom subclass of DocumentSourceMatch which does not serialize itself (since it came from an
 * alias) and requires itself to be the first stage in the pipeline.
 */
class DocumentSourceOplogMatch final : public DocumentSourceMatch {
public:
    static intrusive_ptr<DocumentSourceOplogMatch> create(
        BSONObj filter, const intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceOplogMatch(std::move(filter), expCtx);
    }

    const char* getSourceName() const final {
        // This is used in error reporting, particularly if we find this stage in a position other
        // than first, so report the name as $changeNotification.
        return "$changeNotification";
    }

    StageConstraints constraints() const final {
        StageConstraints constraints;
        constraints.requiredPosition = StageConstraints::PositionRequirement::kFirst;
        constraints.isAllowedInsideFacetStage = false;
        return constraints;
    }

    /**
     * Only serialize this stage for explain purposes, otherwise keep it hidden so that we can
     * properly alias.
     */
    Value serialize(optional<ExplainOptions::Verbosity> explain) const final {
        if (explain) {
            return Value(Document{{kOplogMatchExplainName, Document{}}});
        }
        return Value();
    }

private:
    DocumentSourceOplogMatch(BSONObj filter, const intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMatch(std::move(filter), expCtx) {}
};
}  // namespace

BSONObj DocumentSourceChangeNotification::buildMatchFilter(const NamespaceString& nss) {
    auto target = nss.ns();

    // 1) Supported commands that have the target db namespace (e.g. test.$cmd) in "ns" field.
    auto dropDatabase = BSON("o.dropDatabase" << 1);
    auto dropCollection = BSON("o.drop" << nss.coll());
    auto renameCollection = BSON("o.renameCollection" << target);
    // Commands that are on target db and one of the above.
    auto commandsOnTargetDb =
        BSON("ns" << nss.getCommandNS().ns() << "$or"
                  << BSON_ARRAY(dropDatabase << dropCollection << renameCollection));

    // 2) Supported commands that have arbitrary db namespaces in "ns" field.
    auto renameDropTarget = BSON("o.to" << target);

    // 3) All supported commands that are either (1) or (2).
    auto commandMatch = BSON("op"
                             << "c"
                             << "$or"
                             << BSON_ARRAY(commandsOnTargetDb << renameDropTarget));

    // 4) Normal CRUD ops on the target collection.
    auto opMatch = BSON("ns" << target);

    // Match oplog entries after "start" and are either (3) supported commands or (4) CRUD ops.
    return BSON("ts" << GT << Timestamp() << "$or" << BSON_ARRAY(opMatch << commandMatch));
}

list<intrusive_ptr<DocumentSource>> DocumentSourceChangeNotification::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    // TODO: Add sharding support here (SERVER-29141).
    uassert(40470,
            "The $changeNotification stage is not supported on sharded systems.",
            !expCtx->inRouter);
    uassert(40471,
            "Only default collation is allowed when using a $changeNotification stage.",
            !expCtx->getCollator());

    uassert(40573,
            str::stream() << "the $changeNotification stage must be specified as an object, got "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    bool shouldLookupPostImage = false;
    for (auto&& option : elem.embeddedObject()) {
        auto optionName = option.fieldNameStringData();
        if (optionName == "fullDocument"_sd) {
            uassert(40574,
                    str::stream() << "the 'fullDocument' option to the $changeNotification stage "
                                     "must be a string, got "
                                  << typeName(option.type()),
                    option.type() == BSONType::String);
            auto fullDocOption = option.valueStringData();
            uassert(40575,
                    str::stream() << "unrecognized value for the 'fullDocument' option to the "
                                     "$changeNotification stage. Expected \"none\" or "
                                     "\"fullDocument\", got \""
                                  << option.String()
                                  << "\"",
                    fullDocOption == "lookup"_sd || fullDocOption == "none"_sd);
            shouldLookupPostImage = (fullDocOption == "lookup"_sd);
        } else if (optionName == "resumeAfter"_sd) {
            uasserted(
                40576,
                "the 'resumeAfter' option to the $changeNotification stage is not yet supported");
        } else {
            uasserted(40577,
                      str::stream() << "unrecognized option to $changeNotification stage: \""
                                    << optionName
                                    << "\"");
        }
    }

    auto oplogMatch = DocumentSourceOplogMatch::create(buildMatchFilter(expCtx->ns), expCtx);
    auto transformation = createTransformationStage(elem.embeddedObject(), expCtx);
    list<intrusive_ptr<DocumentSource>> stages = {oplogMatch, transformation};
    if (shouldLookupPostImage) {
        stages.push_back(DocumentSourceLookupChangePostImage::create(expCtx));
    }
    return stages;
}

intrusive_ptr<DocumentSource> DocumentSourceChangeNotification::createTransformationStage(
    BSONObj changeNotificationSpec, const intrusive_ptr<ExpressionContext>& expCtx) {
    return intrusive_ptr<DocumentSource>(new DocumentSourceSingleDocumentTransformation(
        expCtx, stdx::make_unique<Transformation>(changeNotificationSpec), kStageName.toString()));
}

namespace {
void checkValueType(Value v, StringData filedName, BSONType expectedType) {
    uassert(40532,
            str::stream() << "Oplog entry field \"" << filedName << "\" should be "
                          << typeName(expectedType)
                          << ", found: "
                          << typeName(v.getType()),
            (v.getType() == expectedType));
}
}  // namespace

Document DocumentSourceChangeNotification::Transformation::applyTransformation(
    const Document& input) {

    MutableDocument doc;

    // Extract the fields we need.
    checkValueType(input[repl::OplogEntry::kOpTypeFieldName],
                   repl::OplogEntry::kOpTypeFieldName,
                   BSONType::String);
    string op = input[repl::OplogEntry::kOpTypeFieldName].getString();
    Value ts = input[repl::OplogEntry::kTimestampFieldName];
    Value ns = input[repl::OplogEntry::kNamespaceFieldName];
    checkValueType(ns, repl::OplogEntry::kNamespaceFieldName, BSONType::String);
    NamespaceString nss(ns.getString());
    Value id = input.getNestedField("o._id");
    // Non-replace updates have the _id in field "o2".
    Value documentId = id.missing() ? input.getNestedField("o2._id") : id;
    StringData operationType;
    Value fullDocument = Value(BSONNULL);
    Value updateDescription;

    // Deal with CRUD operations and commands.
    auto opType = repl::OpType_parse(IDLParserErrorContext("ChangeNotificationEntry.op"), op);
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

    // Construct the result document. Note that 'documentId' might be the missing value, in which
    // case it will not appear in the output.
    doc.addField(
        kIdField,
        Value(Document{{kTimestmapField, ts}, {kNamespaceField, ns}, {kIdField, documentId}}));
    doc.addField(kOperationTypeField, Value(operationType));
    doc.addField(kFullDocumentField, fullDocument);

    // "invalidate" entry has fewer fields.
    if (opType == repl::OpTypeEnum::kCommand) {
        return doc.freeze();
    }

    doc.addField(kNamespaceField, Value(Document{{"db", nss.db()}, {"coll", nss.coll()}}));
    doc.addField(kDocumentKeyField, Value(Document{{kIdField, documentId}}));

    // Note that 'updateDescription' might be the 'missing' value, in which case it will not be
    // serialized.
    doc.addField("updateDescription", updateDescription);
    return doc.freeze();
}

Document DocumentSourceChangeNotification::Transformation::serializeStageOptions(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Document(_changeNotificationSpec);
}

DocumentSource::GetDepsReturn DocumentSourceChangeNotification::Transformation::addDependencies(
    DepsTracker* deps) const {
    deps->fields.insert(repl::OplogEntry::kOpTypeFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kTimestampFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kNamespaceFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kObjectFieldName.toString());
    deps->fields.insert(repl::OplogEntry::kObject2FieldName.toString());
    return DocumentSource::GetDepsReturn::EXHAUSTIVE_ALL;
}

DocumentSource::GetModPathsReturn
DocumentSourceChangeNotification::Transformation::getModifiedPaths() const {
    // All paths are modified.
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, std::set<string>{}, {}};
}

}  // namespace mongo
