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
 */

#include "mongo/db/commands/privilege_parser.h"

#include "mongo/db/field_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const BSONField<bool> ParsedResource::cluster("cluster");
    const BSONField<string> ParsedResource::db("db");
    const BSONField<string> ParsedResource::collection("collection");

    ParsedResource::ParsedResource() {
        clear();
    }

    ParsedResource::~ParsedResource() {
    }

    bool ParsedResource::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        if (!isClusterSet() && (!isDbSet() || !isCollectionSet())) {
            *errMsg = stream() << "resource must have " << db.name() << " and " <<
                    collection.name() << " set, or must have " << cluster.name();
            return false;
        }
        if (isClusterSet() && (isDbSet() || isCollectionSet())) {
            *errMsg = stream() << "resource cannot have " << cluster.name() << " set as well as "
                    << db.name() << " or " << collection.name();
            return false;
        }
        if (isClusterSet() && !getCluster()) {
            *errMsg = stream() << cluster.name() << " must be true when specified";
            return false;
        }
        if (isDbSet() && (!NamespaceString::validDBName(getDb()) && !getDb().empty())) {
            *errMsg = stream() << getDb() << " is not a valid database name";
            return false;
        }
        if (isCollectionSet() && (!NamespaceString::validCollectionName(getCollection()) &&
                                  !getCollection().empty())) {
            *errMsg = stream() << getCollection() << " is not a valid collection name";
            return false;
        }
        return true;
    }

    BSONObj ParsedResource::toBSON() const {
        BSONObjBuilder builder;

        if (_isClusterSet) builder.append(cluster(), _cluster);

        if (_isDbSet) builder.append(db(), _db);

        if (_isCollectionSet) builder.append(collection(), _collection);

        return builder.obj();
    }

    bool ParsedResource::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, cluster, &_cluster, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isClusterSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, db, &_db, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isDbSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, collection, &_collection, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isCollectionSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void ParsedResource::clear() {
        _cluster = false;
        _isClusterSet = false;

        _db.clear();
        _isDbSet = false;

        _collection.clear();
        _isCollectionSet = false;

    }

    void ParsedResource::cloneTo(ParsedResource* other) const {
        other->clear();

        other->_cluster = _cluster;
        other->_isClusterSet = _isClusterSet;

        other->_db = _db;
        other->_isDbSet = _isDbSet;

        other->_collection = _collection;
        other->_isCollectionSet = _isCollectionSet;
    }

    std::string ParsedResource::toString() const {
        return toBSON().toString();
    }

    void ParsedResource::setCluster(bool cluster) {
        _cluster = cluster;
        _isClusterSet = true;
    }

    void ParsedResource::unsetCluster() {
         _isClusterSet = false;
     }

    bool ParsedResource::isClusterSet() const {
         return _isClusterSet;
    }

    bool ParsedResource::getCluster() const {
        dassert(_isClusterSet);
        return _cluster;
    }

    void ParsedResource::setDb(const StringData& db) {
        _db = db.toString();
        _isDbSet = true;
    }

    void ParsedResource::unsetDb() {
         _isDbSet = false;
     }

    bool ParsedResource::isDbSet() const {
         return _isDbSet;
    }

    const std::string& ParsedResource::getDb() const {
        dassert(_isDbSet);
        return _db;
    }

    void ParsedResource::setCollection(const StringData& collection) {
        _collection = collection.toString();
        _isCollectionSet = true;
    }

    void ParsedResource::unsetCollection() {
         _isCollectionSet = false;
     }

    bool ParsedResource::isCollectionSet() const {
         return _isCollectionSet;
    }

    const std::string& ParsedResource::getCollection() const {
        dassert(_isCollectionSet);
        return _collection;
    }

    const BSONField<std::vector<string> > ParsedPrivilege::actions("actions");
    const BSONField<ParsedResource> ParsedPrivilege::resource("resource");

    ParsedPrivilege::ParsedPrivilege() {
        clear();
    }

    ParsedPrivilege::~ParsedPrivilege() {
    }

    bool ParsedPrivilege::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isActionsSet) {
            *errMsg = stream() << "missing " << actions.name() << " field";
            return false;
        }

        if (!_isResourceSet) {
            *errMsg = stream() << "missing " << resource.name() << " field";
            return false;
        }

        return getResource().isValid(errMsg);
    }

    BSONObj ParsedPrivilege::toBSON() const {
        BSONObjBuilder builder;

        if (_isActionsSet) {
            for (std::vector<string>::const_iterator it = _actions.begin();
                 it != _actions.end();
                 ++it) {
                builder.append(actions(), (*it));
            }
        }

        if (_isResourceSet) builder.append(resource(), _resource.toBSON());

        return builder.obj();
    }

    bool ParsedPrivilege::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, actions, &_actions, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isActionsSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, resource, &_resource, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isResourceSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void ParsedPrivilege::clear() {
        _actions.clear();
        _isActionsSet = false;
        _resource.clear();
        _isResourceSet = false;

    }

    void ParsedPrivilege::cloneTo(ParsedPrivilege* other) const {
        other->clear();

        for(std::vector<string>::const_iterator it = _actions.begin();
            it != _actions.end();
            ++it) {
            other->addToActions(*it);
        }
        other->_isActionsSet = _isActionsSet;

        _resource.cloneTo(&other->_resource);
        other->_isResourceSet = _isResourceSet;
    }

    std::string ParsedPrivilege::toString() const {
        return toBSON().toString();
    }

    void ParsedPrivilege::setActions(const std::vector<string>& actions) {
        for (std::vector<string>::const_iterator it = actions.begin();
             it != actions.end();
             ++it) {
            addToActions((*it));
        }
        _isActionsSet = actions.size() > 0;
    }

    void ParsedPrivilege::addToActions(const string& actions) {
        _actions.push_back(actions);
        _isActionsSet = true;
    }

    void ParsedPrivilege::unsetActions() {
        _actions.clear();
        _isActionsSet = false;
    }

    bool ParsedPrivilege::isActionsSet() const {
        return _isActionsSet;
    }

    size_t ParsedPrivilege::sizeActions() const {
        return _actions.size();
    }

    const std::vector<string>& ParsedPrivilege::getActions() const {
        dassert(_isActionsSet);
        return _actions;
    }

    const string& ParsedPrivilege::getActionsAt(size_t pos) const {
        dassert(_isActionsSet);
        dassert(_actions.size() > pos);
        return _actions.at(pos);
    }

    void ParsedPrivilege::setResource(const ParsedResource& resource) {
        resource.cloneTo(&_resource);
        _isResourceSet = true;
    }

    void ParsedPrivilege::unsetResource() {
         _isResourceSet = false;
     }

    bool ParsedPrivilege::isResourceSet() const {
         return _isResourceSet;
    }

    const ParsedResource& ParsedPrivilege::getResource() const {
        dassert(_isResourceSet);
        return _resource;
    }

} // namespace mongo
