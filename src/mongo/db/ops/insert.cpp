// insert.cpp

/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/ops/insert.h"
#include "mongo/db/global_timestamp.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

using namespace mongoutils;

StatusWith<BSONObj> fixDocumentForInsert(const BSONObj& doc) {
    if (doc.objsize() > BSONObjMaxUserSize)
        return StatusWith<BSONObj>(ErrorCodes::BadValue,
                                   str::stream() << "object to insert too large"
                                                 << ". size in bytes: "
                                                 << doc.objsize()
                                                 << ", max size: "
                                                 << BSONObjMaxUserSize);

    bool firstElementIsId = false;
    bool hasTimestampToFix = false;
    bool hadId = false;
    {
        BSONObjIterator i(doc);
        for (bool isFirstElement = true; i.more(); isFirstElement = false) {
            BSONElement e = i.next();

            if (e.type() == bsonTimestamp && e.timestampValue() == 0) {
                // we replace Timestamp(0,0) at the top level with a correct value
                // in the fast pass, we just mark that we want to swap
                hasTimestampToFix = true;
            }

            auto fieldName = e.fieldNameStringData();

            if (fieldName[0] == '$') {
                return StatusWith<BSONObj>(
                    ErrorCodes::BadValue,
                    str::stream() << "Document can't have $ prefixed field names: " << fieldName);
            }

            // check no regexp for _id (SERVER-9502)
            // also, disallow undefined and arrays
            // Make sure _id isn't duplicated (SERVER-19361).
            if (fieldName == "_id") {
                if (e.type() == RegEx) {
                    return StatusWith<BSONObj>(ErrorCodes::BadValue, "can't use a regex for _id");
                }
                if (e.type() == Undefined) {
                    return StatusWith<BSONObj>(ErrorCodes::BadValue,
                                               "can't use a undefined for _id");
                }
                if (e.type() == Array) {
                    return StatusWith<BSONObj>(ErrorCodes::BadValue, "can't use an array for _id");
                }
                if (e.type() == Object) {
                    BSONObj o = e.Obj();
                    Status s = o.storageValidEmbedded();
                    if (!s.isOK())
                        return StatusWith<BSONObj>(s);
                }
                if (hadId) {
                    return StatusWith<BSONObj>(ErrorCodes::BadValue,
                                               "can't have multiple _id fields in one document");
                } else {
                    hadId = true;
                    firstElementIsId = isFirstElement;
                }
            }
        }
    }

    if (firstElementIsId && !hasTimestampToFix)
        return StatusWith<BSONObj>(BSONObj());

    BSONObjIterator i(doc);

    BSONObjBuilder b(doc.objsize() + 16);
    if (firstElementIsId) {
        b.append(doc.firstElement());
        i.next();
    } else {
        BSONElement e = doc["_id"];
        if (e.type()) {
            b.append(e);
        } else {
            b.appendOID("_id", NULL, true);
        }
    }

    while (i.more()) {
        BSONElement e = i.next();
        if (hadId && e.fieldNameStringData() == "_id") {
            // no-op
        } else if (e.type() == bsonTimestamp && e.timestampValue() == 0) {
            b.append(e.fieldName(), getNextGlobalTimestamp());
        } else {
            b.append(e);
        }
    }
    return StatusWith<BSONObj>(b.obj());
}

Status userAllowedWriteNS(StringData ns) {
    return userAllowedWriteNS(nsToDatabaseSubstring(ns), nsToCollectionSubstring(ns));
}

Status userAllowedWriteNS(const NamespaceString& ns) {
    return userAllowedWriteNS(ns.db(), ns.coll());
}

Status userAllowedWriteNS(StringData db, StringData coll) {
    if (coll == "system.profile") {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "cannot write to '" << db << ".system.profile'");
    }
    return userAllowedCreateNS(db, coll);
}

Status userAllowedCreateNS(StringData db, StringData coll) {
    // validity checking

    if (db.size() == 0)
        return Status(ErrorCodes::BadValue, "db cannot be blank");

    if (!NamespaceString::validDBName(db, NamespaceString::DollarInDbNameBehavior::Allow))
        return Status(ErrorCodes::BadValue, "invalid db name");

    if (coll.size() == 0)
        return Status(ErrorCodes::BadValue, "collection cannot be blank");

    if (!NamespaceString::validCollectionName(coll))
        return Status(ErrorCodes::BadValue, "invalid collection name");

    if (db.size() + 1 /* dot */ + coll.size() > NamespaceString::MaxNsCollectionLen)
        return Status(ErrorCodes::BadValue,
                      str::stream() << "fully qualified namespace " << db << '.' << coll
                                    << " is too long "
                                    << "(max is "
                                    << NamespaceString::MaxNsCollectionLen
                                    << " bytes)");

    // check spceial areas

    if (db == "system")
        return Status(ErrorCodes::BadValue, "cannot use 'system' database");


    if (coll.startsWith("system.")) {
        if (coll == "system.indexes")
            return Status::OK();
        if (coll == "system.js")
            return Status::OK();
        if (coll == "system.profile")
            return Status::OK();
        if (coll == "system.users")
            return Status::OK();
        if (db == "admin") {
            if (coll == "system.version")
                return Status::OK();
            if (coll == "system.roles")
                return Status::OK();
            if (coll == "system.new_users")
                return Status::OK();
            if (coll == "system.backup_users")
                return Status::OK();
        }
        if (db == "local") {
            if (coll == "system.replset")
                return Status::OK();
        }
        return Status(ErrorCodes::BadValue,
                      str::stream() << "cannot write to '" << db << "." << coll << "'");
    }

    // some special rules

    if (coll.find(".system.") != string::npos) {
        // this matches old (2.4 and older) behavior, but I'm not sure its a good idea
        return Status(ErrorCodes::BadValue,
                      str::stream() << "cannot write to '" << db << "." << coll << "'");
    }

    return Status::OK();
}
}
