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

#include "mongo/db/ops/modifier_object_replace.h"

#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/global_timestamp.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace str = mongoutils::str;

namespace {
const char idFieldName[] = "_id";

Status fixupTimestamps(const BSONObj& obj) {
    BSONObjIterator i(obj);
    while (i.more()) {
        BSONElement e = i.next();

        // Skip _id field -- we do not replace it
        if (e.type() == bsonTimestamp && e.fieldNameStringData() != idFieldName) {
            auto timestampView = DataView(const_cast<char*>(e.value()));

            // We don't need to do an endian-safe read here, because 0 is 0 either way.
            unsigned long long timestamp = timestampView.read<unsigned long long>();
            if (timestamp == 0) {
                // performance note, this locks a mutex:
                Timestamp ts(getNextGlobalTimestamp());
                timestampView.write(tagLittleEndian(ts.asULL()));
            }
        }
    }

    return Status::OK();
}
}

struct ModifierObjectReplace::PreparedState {
    PreparedState(mutablebson::Document* targetDoc) : doc(*targetDoc), noOp(false) {}

    // Document that is going to be changed
    mutablebson::Document& doc;

    // This is a no op
    bool noOp;
};

ModifierObjectReplace::ModifierObjectReplace() : _val() {}

ModifierObjectReplace::~ModifierObjectReplace() {}

Status ModifierObjectReplace::init(const BSONElement& modExpr,
                                   const Options& opts,
                                   bool* positional) {
    if (modExpr.type() != Object) {
        // Impossible, really since the caller check this already...
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Document replacement expects a complete document"
                                       " but the type supplied was "
                                    << modExpr.type());
    }

    // Object replacements never have positional operator.
    if (positional)
        *positional = false;

    // We make a copy of the object here because the update driver does not guarantees, in
    // the case of object replacement, that the modExpr is going to outlive this mod.
    _val = modExpr.embeddedObject().getOwned();
    return fixupTimestamps(_val);
}

Status ModifierObjectReplace::prepare(mutablebson::Element root,
                                      StringData matchedField,
                                      ExecInfo* execInfo) {
    _preparedState.reset(new PreparedState(&root.getDocument()));

    // objectSize checked by binaryEqual (optimization)
    BSONObj objOld = root.getDocument().getObject();
    if (objOld.binaryEqual(_val)) {
        _preparedState->noOp = true;
        execInfo->noOp = true;
    }

    return Status::OK();
}

Status ModifierObjectReplace::apply() const {
    dassert(!_preparedState->noOp);

    // Remove the contents of the provided doc.
    mutablebson::Document& doc = _preparedState->doc;
    mutablebson::Element current = doc.root().leftChild();
    mutablebson::Element srcIdElement = doc.end();
    while (current.ok()) {
        mutablebson::Element toRemove = current;
        current = current.rightSibling();

        // Skip _id field element -- it should not change
        if (toRemove.getFieldName() == idFieldName) {
            srcIdElement = toRemove;
            continue;
        }

        Status status = toRemove.remove();
        if (!status.isOK()) {
            return status;
        }
    }

    // Insert the provided contents instead.
    BSONElement dstIdElement;
    BSONObjIterator it(_val);
    while (it.more()) {
        BSONElement elem = it.next();
        if (elem.fieldNameStringData() == idFieldName) {
            dstIdElement = elem;

            // Do not duplicate _id field
            if (srcIdElement.ok()) {
                if (srcIdElement.compareWithBSONElement(dstIdElement, true) != 0) {
                    return Status(ErrorCodes::ImmutableField,
                                  str::stream() << "The _id field cannot be changed from {"
                                                << srcIdElement.toString()
                                                << "} to {"
                                                << dstIdElement.toString()
                                                << "}.");
                }
                continue;
            }
        }

        Status status = doc.root().appendElement(elem);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status ModifierObjectReplace::log(LogBuilder* logBuilder) const {
    mutablebson::Document& doc = logBuilder->getDocument();

    mutablebson::Element replacementObject = doc.end();
    Status status = logBuilder->getReplacementObject(&replacementObject);

    if (status.isOK()) {
        mutablebson::Element current = _preparedState->doc.root().leftChild();
        while (current.ok()) {
            status = replacementObject.appendElement(current.getValue());
            if (!status.isOK())
                return status;
            current = current.rightSibling();
        }
    }

    return status;
}

}  // namespace mongo
