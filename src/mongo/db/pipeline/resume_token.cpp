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

#include "mongo/db/pipeline/resume_token.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_sources_gen.h"
#include "mongo/db/pipeline/value_comparator.h"

namespace mongo {

ResumeToken::ResumeToken(const BSONObj& resumeBson) {
    auto token = ResumeTokenInternal::parse(
        IDLParserErrorContext("$changeNotification.resumeAfter"), resumeBson);
    _timestamp = token.getTimestamp();
    _namespace = token.getNs().toString();
    _documentId = token.getDocumentId();
}

ResumeToken::ResumeToken(const Value& resumeValue) {
    Document resumeTokenDoc = resumeValue.getDocument();
    Value timestamp = resumeTokenDoc[ResumeTokenInternal::kTimestampFieldName];
    _timestamp = timestamp.getTimestamp();
    Value ns = resumeTokenDoc[ResumeTokenInternal::kNsFieldName];
    _namespace = ns.getString();
    _documentId = resumeTokenDoc[ResumeTokenInternal::kDocumentIdFieldName];
}

bool ResumeToken::operator==(const ResumeToken& other) {
    return _timestamp == other._timestamp && _namespace == other._namespace &&
        ValueComparator::kInstance.evaluate(_documentId == other._documentId);
}

Document ResumeToken::toDocument() const {
    return Document({{ResumeTokenInternal::kTimestampFieldName, _timestamp},
                     {{ResumeTokenInternal::kNsFieldName}, _namespace},
                     {{ResumeTokenInternal::kDocumentIdFieldName}, _documentId}});
}

BSONObj ResumeToken::toBSON() const {
    return BSON(
        ResumeTokenInternal::kTimestampFieldName << _timestamp << ResumeTokenInternal::kNsFieldName
                                                 << _namespace
                                                 << ResumeTokenInternal::kDocumentIdFieldName
                                                 << _documentId);
}

ResumeToken ResumeToken::parse(const BSONObj& resumeBson) {
    return ResumeToken(resumeBson);
}

}  // namespace mongo
