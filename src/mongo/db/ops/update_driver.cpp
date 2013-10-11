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

#include "mongo/db/ops/update_driver.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/ops/modifier_object_replace.h"
#include "mongo/db/ops/modifier_table.h"
#include "mongo/db/ops/path_support.h"
#include "mongo/util/embedded_builder.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    namespace str = mongoutils::str;

    UpdateDriver::UpdateDriver(const Options& opts)
        : _multi(opts.multi)
        , _upsert(opts.upsert)
        , _logOp(opts.logOp)
        , _modOptions(opts.modOptions) {
    }

    UpdateDriver::~UpdateDriver() {
        clear();
    }

    Status UpdateDriver::parse(const BSONObj& updateExpr) {
        clear();

        // Check if the update expression is a full object replacement.
        if (*updateExpr.firstElementFieldName() != '$') {
            if (_multi) {
                return Status(ErrorCodes::FailedToParse,
                              "multi update only works with $ operators");
            }

            // Modifiers expect BSONElements as input. But the input to object replace is, by
            // definition, an object. We wrap the 'updateExpr' as the mod is expecting. Note
            // that the wrapper is temporary so the object replace mod should make a copy of
            // the object.
            auto_ptr<ModifierObjectReplace> mod(new ModifierObjectReplace);
            BSONObj wrapper = BSON( "dummy" << updateExpr );
            Status status = mod->init(wrapper.firstElement(), _modOptions);
            if (!status.isOK()) {
                return status;
            }

            _mods.push_back(mod.release());

            // Register the fact that this driver will only do full object replacements.
            _replacementMode = true;

            return Status::OK();
        }

        // The update expression is made of mod operators, that is
        // { <$mod>: {...}, <$mod>: {...}, ...  }
        BSONObjIterator outerIter(updateExpr);
        while (outerIter.more()) {
            BSONElement outerModElem = outerIter.next();

            // Check whether this is a valid mod type.
            modifiertable::ModifierType modType = modifiertable::getType(outerModElem.fieldName());
            if (modType == modifiertable::MOD_UNKNOWN) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "Unknown modifier: " << outerModElem.fieldName());
            }

            // Check whether there is indeed a list of mods under this modifier.
            if (outerModElem.type() != Object) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "List of $mods must be an embedded document"
                                                  " but it is a  "
                                            << typeName(outerModElem.type())
                                            << " instead.");
            }

            // Check whether there are indeed mods under this modifier.
            if (outerModElem.embeddedObject().isEmpty()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << outerModElem.fieldName()
                                            << " is empty. You must specify a field like so: "
                                                    "{$mod: {<field>: ...}}");
            }

            BSONObjIterator innerIter(outerModElem.embeddedObject());
            while (innerIter.more()) {
                BSONElement innerModElem = innerIter.next();

                if (innerModElem.eoo()) {
                    return Status(ErrorCodes::FailedToParse,
                                  str::stream() << outerModElem.fieldName()
                                                << "." << innerModElem.fieldName()
                                                << " has no value: " << innerModElem
                                                << " which is not allowed for any $<mod>.");
                }

                auto_ptr<ModifierInterface> mod(modifiertable::makeUpdateMod(modType));
                dassert(mod.get());

                Status status = mod->init(innerModElem, _modOptions);
                if (!status.isOK()) {
                    return status;
                }

                _mods.push_back(mod.release());
            }
        }

        // Register the fact that there will be only $mod's in this driver -- no object
        // replacement.
        _replacementMode = false;

        return Status::OK();
    }

    Status UpdateDriver::createFromQuery(const BSONObj& query, mutablebson::Document& doc) {
        BSONObjIteratorSorted i(query);
        while (i.more()) {
            BSONElement e = i.next();
            // TODO: get this logic/exclude-list from the query system?
            if (e.fieldName()[0] == '$' || e.fieldNameStringData() == "_id")
                continue;


            if (e.type() == Object && e.embeddedObject().firstElementFieldName()[0] == '$') {
                // we have something like { x : { $gt : 5 } }
                // this can be a query piece
                // or can be a dbref or something

                int op = e.embeddedObject().firstElement().getGtLtOp();
                if (op > 0) {
                    // This means this is a $gt type filter, so don't make it part of the new
                    // object.
                    continue;
                }

                if (mongoutils::str::equals(e.embeddedObject().firstElement().fieldName(),
                                              "$not")) {
                    // A $not filter operator is not detected in getGtLtOp() and should not
                    // become part of the new object.
                    continue;
                }
            }

            // Add to the field to doc after expanding and checking for conflicts.
            FieldRef elemName;
            const StringData& elemNameSD(e.fieldNameStringData());
            elemName.parse(elemNameSD);

            size_t pos;
            mutablebson::Element* elemFound = NULL;

            Status status = pathsupport::findLongestPrefix(elemName, doc.root(), &pos, elemFound);
            // Not NonExistentPath, of OK, return
            if (!(status.code() == ErrorCodes::NonExistentPath || status.isOK()))
                return status;

            status = pathsupport::createPathAt(elemName,
                                               0,
                                               doc.root(),
                                               doc.makeElementWithNewFieldName(
                                                       elemName.getPart(elemName.numParts()-1),
                                                       e));
            if (!status.isOK())
                return status;
        }
        return Status::OK();
    }

    Status UpdateDriver::update(const StringData& matchedField,
                                mutablebson::Document* doc,
                                BSONObj* logOpRec) {
        // TODO: assert that update() is called at most once in a !_multi case.

        FieldRefSet targetFields;

        _affectIndices = false;

        if (_shardKeyState)
            _shardKeyState->affectedKeySet.clear();

        _logDoc.reset();
        LogBuilder logBuilder(_logDoc.root());

        // Ask each of the mods to type check whether they can operate over the current document
        // and, if so, to change that document accordingly.
        for (vector<ModifierInterface*>::iterator it = _mods.begin(); it != _mods.end(); ++it) {
            ModifierInterface::ExecInfo execInfo;
            Status status = (*it)->prepare(doc->root(), matchedField, &execInfo);
            if (!status.isOK()) {
                return status;
            }

            // If a mod wants to be applied only if this is an upsert (or only if this is a
            // strict update), we should respect that. If a mod doesn't care, it would state
            // it is fine with ANY update context.
            const bool validContext = (execInfo.context == ModifierInterface::ExecInfo::ANY_CONTEXT ||
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

                if (!execInfo.noOp && _shardKeyState)
                    _shardKeyState->keySet.getConflicts(
                        execInfo.fieldRef[i],
                        &_shardKeyState->affectedKeySet);

                if (!targetFields.empty() || _mods.size() > 1) {
                    const FieldRef* other;
                    if (!targetFields.insert(execInfo.fieldRef[i], &other)) {
                        return Status(ErrorCodes::ConflictingUpdateOperators,
                                      str::stream() << "Cannot update '"
                                                    << other->dottedField()
                                                    << "' and '"
                                                    << execInfo.fieldRef[i]->dottedField()
                                                    << "' at the same time");
                    }
                }

                // We start with the expectation that a mod will be in-place. But if the mod
                // touched an indexed field and the mod will indeed be executed -- that is, it
                // is not a no-op and it is in a valid context -- then we switch back to a
                // non-in-place mode.
                //
                // TODO: make mightBeIndexed and fieldRef like each other.
                if (!_affectIndices &&
                    !execInfo.noOp &&
                    _indexedFields.mightBeIndexed(execInfo.fieldRef[i]->dottedField())) {
                    _affectIndices = true;
                    doc->disableInPlaceUpdates();
                }
            }

            if (!execInfo.noOp) {
                status = (*it)->apply();
                if (!status.isOK()) {
                    return status;
                }
            }

            // If we require a replication oplog entry for this update, go ahead and generate one.
            if (_logOp && logOpRec) {
                status = (*it)->log(&logBuilder);
                if (!status.isOK()) {
                    return status;
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

    void UpdateDriver::refreshIndexKeys(const IndexPathSet& indexedFields) {
        _indexedFields = indexedFields;
    }

    void UpdateDriver::refreshShardKeyPattern(const BSONObj& shardKeyPattern) {

        // An empty pattern object means no shard keys.
        if (shardKeyPattern.isEmpty()) {
            _shardKeyState.reset();
            return;
        }

        // If we have already parsed an identical shard key pattern, don't do it again.
        if (_shardKeyState && (_shardKeyState->pattern.woCompare(shardKeyPattern) == 0))
            return;

        // Reset the shard key state and capture the new pattern.
        _shardKeyState.reset(new ShardKeyState);
        _shardKeyState->pattern = shardKeyPattern;

        // Parse the shard keys into the states 'keys' and 'keySet' members.
        BSONObjIterator patternIter = _shardKeyState->pattern.begin();
        while (patternIter.more())  {
            BSONElement current = patternIter.next();

            _shardKeyState->keys.mutableVector().push_back(new FieldRef);
            FieldRef* const newFieldRef = _shardKeyState->keys.mutableVector().back();
            newFieldRef->parse(current.fieldNameStringData());

            // TODO: what about bad parse?

            const FieldRef* conflict;
            if ( !_shardKeyState->keySet.insert( newFieldRef, &conflict ) ) {
                // This seems pretty unlikely in practice.
                uasserted(
                    17152, str::stream()
                    << "Shard key '"
                    << newFieldRef->dottedField()
                    << "' conflicts with shard key '"
                    << conflict->dottedField()
                    << "'" );
            }
        }
    }

    bool UpdateDriver::modsAffectShardKeys() const {
        // If we have no shard key state, the mods could not have affected them.
        if (!_shardKeyState)
            return false;

        // In replacement mode, we always assume that all shard keys need to be checked. For
        // upsert, we are inserting a new object, so it must have all shard keys. Otherwise, it
        // depends on whether any shard keys were added to the affectedKeySet state.
        return (_replacementMode || !_shardKeyState->affectedKeySet.empty());
    }

    Status UpdateDriver::checkShardKeysUnaltered(const BSONObj& original,
                                                 const mutablebson::Document& updated) const {

        if (!_shardKeyState)
            return Status::OK();

        // In replacement mode, we validate the values for all shard keys. Otherwise, only the
        // ones tagged as being potentially invalidated. For an upsert, we check all keys.
        const FieldRefSet& affected = (_replacementMode || original.isEmpty()) ?
            _shardKeyState->keySet :
            _shardKeyState->affectedKeySet;

        const mutablebson::ConstElement id = updated.root()["_id"];

        FieldRefSet::const_iterator where = affected.begin();
        const FieldRefSet::const_iterator end = affected.end();
        for( ; where != end; ++where ) {
            const FieldRef& current = **where;

            // Find the affected field in the updated document.
            mutablebson::ConstElement elt = updated.root();
            size_t currentPart = 0;
            while (elt.ok() && currentPart < current.numParts())
                elt = elt[current.getPart(currentPart++)];

            if (!elt.ok()) {
                if (original.isEmpty()) {
                    return Status(ErrorCodes::NoSuchKey,
                                  mongoutils::str::stream()
                                  << "After applying modifiers of replacement in an upsert"
                                  << "', the shard key field '" << current.dottedField() <<
                                  "' was not found in the resulting document.");

                }
                else {
                    return Status(ErrorCodes::ImmutableShardKeyField,
                                  mongoutils::str::stream()
                                  << "After applying updates to the object with _id '"
                                  << id.toString()
                                  << "', the shard key field '" << current.dottedField() <<
                                  "' was found to have been removed from the document");
                }
            }

            // For upserts, we don't have an original object to compare against. The existence
            // check above must be sufficient for now. Potentially, we could do keyBelongsTo me
            // here.
            //
            // TODO: Investigate calling keyBelongsToMe here. We'd need to do this if we want
            // mongod to be a full backstop for incorrect updates in a forward compatible way,
            // looking at the day where we open up the floodgates for updates through mongod.
            if (!original.isEmpty()) {

                // Find the potentially affected field in the original document.
                const BSONElement foundInOld = original.getFieldDotted(current.dottedField());
                if (foundInOld.eoo()) {

                    // NOTE: These errors should really never occur. It would mean that the base
                    // document on disk was missing the shard key, so we have no way to validate
                    // that it wasn't changed.

                    BSONElement id;
                    if (original.getObjectID(id)) {
                        return Status(ErrorCodes::NoSuchKey,
                                      mongoutils::str::stream()
                                      << "While updating object with _id '" << id << "', the field"
                                      << " for shard key '"
                                      << current.dottedField() << "' was not found "
                                      << "in the original document");
                    }
                    else {
                        return Status(ErrorCodes::NoSuchKey,
                                      mongoutils::str::stream()
                                      << "While updating an object with no '_id' field, the field"
                                      << " for shard key '"
                                      << current.dottedField() << "' was not found "
                                      << "in the original document");
                    }
                }

                if (elt.compareWithBSONElement(foundInOld, false) != 0) {
                    return Status(ErrorCodes::ImmutableShardKeyField,
                                  mongoutils::str::stream()
                                  << "After applying updates to the object with _id '" <<
                                  id.toString()
                                  << "', the shard key field '" << current.dottedField() <<
                                  "' was found to have been altered");
                }
            }
        }

        return Status::OK();
    }

    bool UpdateDriver::multi() const {
        return _multi;
    }

    void UpdateDriver::setMulti(bool multi) {
        _multi = multi;
    }

    bool UpdateDriver::upsert() const {
        return _upsert;
    }

    void UpdateDriver::setUpsert(bool upsert) {
        _upsert = upsert;
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

    ModifierInterface::ExecInfo::UpdateContext UpdateDriver::context() const {
        return _context;
    }

    void UpdateDriver::setContext(ModifierInterface::ExecInfo::UpdateContext context) {
        _context = context;
    }

    BSONObj UpdateDriver::makeOplogEntryQuery(const BSONObj doc, bool multi) const {
        BSONObjBuilder idPattern;
        BSONElement id;
        // NOTE: If the matching object lacks an id, we'll log
        // with the original pattern.  This isn't replay-safe.
        // It might make sense to suppress the log instead
        // if there's no id.
        if ( doc.getObjectID( id ) ) {
           idPattern.append( id );
           return idPattern.obj();
        }
        else {
           uassert( 16980,
                    str::stream() << "Multi-update operations require all documents to "
                                     "have an '_id' field. " << doc.toString(false, false),
                    ! multi );
           return doc;
        }
    }
    void UpdateDriver::clear() {
        for (vector<ModifierInterface*>::iterator it = _mods.begin(); it != _mods.end(); ++it) {
            delete *it;
        }
        _indexedFields.clear();
        _replacementMode = false;
        _shardKeyState.reset();
    }

} // namespace mongo
