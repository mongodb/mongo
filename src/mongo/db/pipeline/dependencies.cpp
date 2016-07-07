/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::set;
using std::string;
using std::vector;

namespace str = mongoutils::str;

BSONObj DepsTracker::toProjection() const {
    BSONObjBuilder bb;

    if (_needTextScore)
        bb.append(Document::metaFieldTextScore,
                  BSON("$meta"
                       << "textScore"));

    if (needWholeDocument)
        return bb.obj();

    if (fields.empty()) {
        return BSONObj();
    }

    bool needId = false;
    string last;
    for (set<string>::const_iterator it(fields.begin()), end(fields.end()); it != end; ++it) {
        if (str::startsWith(*it, "_id") && (it->size() == 3 || (*it)[3] == '.')) {
            // _id and subfields are handled specially due in part to SERVER-7502
            needId = true;
            continue;
        }

        if (!last.empty() && str::startsWith(*it, last)) {
            // we are including a parent of *it so we don't need to include this field
            // explicitly. In fact, due to SERVER-6527 if we included this field, the parent
            // wouldn't be fully included.  This logic relies on on set iterators going in
            // lexicographic order so that a string is always directly before of all fields it
            // prefixes.
            continue;
        }

        last = *it + '.';
        bb.append(*it, 1);
    }

    if (needId)  // we are explicit either way
        bb.append("_id", 1);
    else
        bb.append("_id", 0);

    return bb.obj();
}

// ParsedDeps::_fields is a simple recursive look-up table. For each field:
//      If the value has type==Bool, the whole field is needed
//      If the value has type==Object, the fields in the subobject are needed
//      All other fields should be missing which means not needed
boost::optional<ParsedDeps> DepsTracker::toParsedDeps() const {
    MutableDocument md;

    if (needWholeDocument || _needTextScore) {
        // can't use ParsedDeps in this case
        return boost::none;
    }

    string last;
    for (set<string>::const_iterator it(fields.begin()), end(fields.end()); it != end; ++it) {
        if (!last.empty() && str::startsWith(*it, last)) {
            // we are including a parent of *it so we don't need to include this field
            // explicitly. In fact, if we included this field, the parent wouldn't be fully
            // included.  This logic relies on on set iterators going in lexicographic order so
            // that a string is always directly before of all fields it prefixes.
            continue;
        }
        last = *it + '.';
        md.setNestedField(*it, Value(true));
    }

    return ParsedDeps(md.freeze());
}

namespace {
// Mutually recursive with arrayHelper
Document documentHelper(const BSONObj& bson, const Document& neededFields);

// Handles array-typed values for ParsedDeps::extractFields
Value arrayHelper(const BSONObj& bson, const Document& neededFields) {
    BSONObjIterator it(bson);

    vector<Value> values;
    while (it.more()) {
        BSONElement bsonElement(it.next());
        if (bsonElement.type() == Object) {
            Document sub = documentHelper(bsonElement.embeddedObject(), neededFields);
            values.push_back(Value(sub));
        }

        if (bsonElement.type() == Array) {
            values.push_back(arrayHelper(bsonElement.embeddedObject(), neededFields));
        }
    }

    return Value(std::move(values));
}

// Handles object-typed values including the top-level for ParsedDeps::extractFields
Document documentHelper(const BSONObj& bson, const Document& neededFields) {
    MutableDocument md(neededFields.size());

    BSONObjIterator it(bson);
    while (it.more()) {
        BSONElement bsonElement(it.next());
        StringData fieldName = bsonElement.fieldNameStringData();
        Value isNeeded = neededFields[fieldName];

        if (isNeeded.missing())
            continue;

        if (isNeeded.getType() == Bool) {
            md.addField(fieldName, Value(bsonElement));
            continue;
        }

        dassert(isNeeded.getType() == Object);

        if (bsonElement.type() == Object) {
            Document sub = documentHelper(bsonElement.embeddedObject(), isNeeded.getDocument());
            md.addField(fieldName, Value(sub));
        }

        if (bsonElement.type() == Array) {
            md.addField(fieldName,
                        arrayHelper(bsonElement.embeddedObject(), isNeeded.getDocument()));
        }
    }

    return md.freeze();
}
}  // namespace

Document ParsedDeps::extractFields(const BSONObj& input) const {
    return documentHelper(input, _fields);
}
}
