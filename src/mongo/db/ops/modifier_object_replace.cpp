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

#include "mongo/base/error_codes.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    namespace {
        // TODO: Egregiously stolen from jsobjmanipulator.h and instance.cpp to break a link
        // dependency cycle. We should sort this out, but we don't need to do it right now.
        void fixupTimestamps( const BSONObj& obj ) {
            BSONObjIterator i(obj);
            for(int j = 0; i.moreWithEOO() && j < 2; ++j) {
                BSONElement e = i.next();
                if (e.eoo())
                    break;
                if (e.type() == Timestamp) {
                    // performance note, this locks a mutex:
                    unsigned long long &timestamp =
                        *(reinterpret_cast<unsigned long long*>(
                              const_cast<char *>(e.value())));
                    if (timestamp == 0) {
                        mutex::scoped_lock lk(OpTime::m);
                        timestamp = OpTime::now(lk).asDate();
                    }
                    break;
                }
            }
        }
    }

    struct ModifierObjectReplace::PreparedState {

        PreparedState(mutablebson::Document* targetDoc)
            : doc(*targetDoc)
            , noOp(false) {
        }

        // Document that is going to be changed
        mutablebson::Document& doc;

        // This is a no op
        bool noOp;

    };

    ModifierObjectReplace::ModifierObjectReplace() : _val() {
    }

    ModifierObjectReplace::~ModifierObjectReplace() {
    }

    Status ModifierObjectReplace::init(const BSONElement& modExpr, const Options& opts) {

        if (modExpr.type() != Object) {
            return Status(ErrorCodes::BadValue, mongoutils::str::stream() <<
                          "object replace expects full object but type was " << modExpr.type());
        }

        if (opts.enforceOkForStorage && !modExpr.embeddedObject().okForStorageAsRoot()) {
            return Status(ErrorCodes::BadValue, "not okForStorage: "
                                                " the document has invalid fields");
        }

        BSONObjIterator it(modExpr.embeddedObject());
        while (it.more()) {
            BSONElement elem = it.next();
            if (*elem.fieldName() == '$') {
                return Status(ErrorCodes::BadValue, "can't mix modifiers and non-modifiers");
            }
        }

        // We make a copy of the object here because the update driver does not guarantees, in
        // the case of object replacement, that the modExpr is going to outlive this mod.
        _val = modExpr.embeddedObject().getOwned();
        fixupTimestamps(_val);

        return Status::OK();
    }

    Status ModifierObjectReplace::prepare(mutablebson::Element root,
                                          const StringData& matchedField,
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
            if (toRemove.getFieldName() == "_id") {
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
            if (elem.fieldNameStringData() == "_id") {
                dstIdElement = elem;

                // Do not duplicate _id field
                if (srcIdElement.ok()) {
                    continue;
                }
            }

            Status status = doc.root().appendElement(elem);
            if (!status.isOK()) {
                return status;
            }
        }

        // Error if the _id changed
        if (srcIdElement.ok() && !dstIdElement.eoo() &&
            (srcIdElement.compareWithBSONElement(dstIdElement) != 0)) {

            return Status(ErrorCodes::CannotMutateObject, "The _id field cannot be changed");
        }

        return Status::OK();
    }

    Status ModifierObjectReplace::log(LogBuilder* logBuilder) const {

        mutablebson::Document& doc = logBuilder->getDocument();

        mutablebson::Element replacementObject = doc.end();
        Status status = logBuilder->getReplacementObject(&replacementObject);

        if (status.isOK()) {
            BSONObjIterator it(_val);
            while (status.isOK() && it.more())
                status = replacementObject.appendElement(it.next());
        }

        return status;
    }

} // namespace mongo
