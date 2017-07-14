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

#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using std::vector;
using std::string;

constexpr StringData kNamespaceField = "ns"_sd;
constexpr StringData kTimestmapField = "ts"_sd;
constexpr StringData kOpTypeField = "op"_sd;
constexpr StringData kOField = "o"_sd;
constexpr StringData kIdField = "_id"_sd;

REGISTER_MULTI_STAGE_ALIAS(changeNotification,
                           DocumentSourceChangeNotification::LiteParsed::parse,
                           DocumentSourceChangeNotification::createFromBson);

BSONObj DocumentSourceChangeNotification::buildMatch(BSONElement elem, const NamespaceString& nss) {
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
    auto matchFilter =
        BSON("ts" << GT << Timestamp() << "$or" << BSON_ARRAY(opMatch << commandMatch));
    return BSON("$match" << matchFilter);
}

vector<intrusive_ptr<DocumentSource>> DocumentSourceChangeNotification::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    // TODO: Add sharding support here (SERVER-29141).
    uassert(40470,
            "The $changeNotification stage is not supported on sharded systems.",
            !expCtx->inRouter);
    uassert(40471,
            "Only default collation is allowed when using a $changeNotification stage.",
            !expCtx->getCollator());

    BSONObj matchObj = buildMatch(elem, expCtx->ns);

    auto matchSource = DocumentSourceMatch::createFromBson(matchObj.firstElement(), expCtx);
    auto transformSource = createTransformationStage(expCtx);
    return {matchSource, transformSource};
}

intrusive_ptr<DocumentSource> DocumentSourceChangeNotification::createTransformationStage(
    const intrusive_ptr<ExpressionContext>& expCtx) {
    return intrusive_ptr<DocumentSource>(new DocumentSourceSingleDocumentTransformation(
        expCtx, stdx::make_unique<Transformation>(), "$changeNotification"));
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
    checkValueType(input[kOpTypeField], kOpTypeField, BSONType::String);
    string op = input[kOpTypeField].getString();
    Value ts = input[kTimestmapField];
    Value ns = input[kNamespaceField];
    checkValueType(ns, kNamespaceField, BSONType::String);
    NamespaceString nss(ns.getString());
    Value id = input.getNestedField("o._id");
    // Non-replace updates have the _id in field "o2".
    Value documentId = id.missing() ? input.getNestedField("o2._id") : id;
    string operationType;
    Value newDocument;
    Value updateDescription;

    // Deal with CRUD operations and commands.
    if (op == "i") {
        operationType = "insert";
        newDocument = input[kOField];
    } else if (op == "d") {
        operationType = "delete";
    } else if (op == "u") {
        if (id.missing()) {
            operationType = "update";
            checkValueType(input[kOField], kOField, BSONType::Object);
            Document o = input[kOField].getDocument();
            Value updatedFields = o["$set"];
            Value removedFields = o["$unset"];

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
            operationType = "replace";
            newDocument = input[kOField];
        }
    } else if (op == "c") {
        operationType = "invalidate";
        // Make sure the result doesn't have a document id.
        documentId = Value();
    }

    // Construct the result document. If document id is missing, it will not appear in the output.
    doc.addField(
        kIdField,
        Value(Document{{kTimestmapField, ts}, {kNamespaceField, ns}, {kIdField, documentId}}));
    doc.addField("operationType", Value(operationType));

    // "invalidate" entry has fewer fields.
    if (op == "c") {
        return doc.freeze();
    }

    // Add fields for normal operations.
    doc.addField(kNamespaceField, Value(Document{{"db", nss.db()}, {"coll", nss.coll()}}));
    doc.addField("documentKey", Value(Document{{kIdField, documentId}}));

    // If newDocument or updateDescription is missing, it will not be serialized.
    doc.addField("newDocument", newDocument);
    doc.addField("updateDescription", updateDescription);
    return doc.freeze();
}

Document DocumentSourceChangeNotification::Transformation::serializeStageOptions(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Document();
}

DocumentSource::GetDepsReturn DocumentSourceChangeNotification::Transformation::addDependencies(
    DepsTracker* deps) const {
    deps->fields.insert(kOpTypeField.toString());
    deps->fields.insert(kTimestmapField.toString());
    deps->fields.insert(kNamespaceField.toString());
    deps->fields.insert(kOField.toString());
    deps->fields.insert("o2");
    return DocumentSource::GetDepsReturn::EXHAUSTIVE_ALL;
}

DocumentSource::GetModPathsReturn
DocumentSourceChangeNotification::Transformation::getModifiedPaths() const {
    // All paths are modified.
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, std::set<string>{}, {}};
}

}  // namespace mongo
