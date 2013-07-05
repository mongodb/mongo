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

#include "mongo/db/ops/update_driver.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/ops/modifier_interface.h"
#include "mongo/db/ops/modifier_object_replace.h"
#include "mongo/db/ops/modifier_table.h"
#include "mongo/util/embedded_builder.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    UpdateDriver::UpdateDriver(const Options& opts)
        : _multi(opts.multi)
        , _upsert(opts.upsert)
        , _logOp(opts.logOp) {
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
            Status status = mod->init(wrapper.firstElement());
            if (!status.isOK()) {
                return status;
            }

            _mods.push_back(mod.release());

            return Status::OK();
        }

        // The update expression is made of mod operators, that is
        // { <$mod>: {...}, <$mod>: {...}, ...  }
        BSONObjIterator outerIter(updateExpr);
        while (outerIter.more()) {
            BSONElement outerModElem = outerIter.next();

            modifiertable::ModifierType modType = modifiertable::getType(outerModElem.fieldName());
            if (modType == modifiertable::MOD_UNKNOWN) {
                return Status(ErrorCodes::FailedToParse, "wrong modifier type");
            }

            BSONObjIterator innerIter(outerModElem.embeddedObject());
            while (innerIter.more()) {
                BSONElement innerModElem = innerIter.next();

                auto_ptr<ModifierInterface> mod(modifiertable::makeUpdateMod(modType));
                dassert(mod.get());

                if (innerModElem.eoo()) {
                    return Status(ErrorCodes::FailedToParse, "empty mod");
                }

                Status status = mod->init(innerModElem);
                if (!status.isOK()) {
                    return status;
                }

                _mods.push_back(mod.release());
            }
        }

        return Status::OK();
    }

    bool UpdateDriver::createFromQuery(const BSONObj query, BSONObj* newObj) const {
        // TODO
        // This moved from ModSet::createNewFromQuery
        // Check if it can be streamlined
        BSONObjBuilder bb;
        EmbeddedBuilder eb(&bb);
        BSONObjIteratorSorted i(query);
        while (i.more()) {
            BSONElement e = i.next();
            if (e.fieldName()[0] == '$') // for $atomic and anything else we add
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

            eb.appendAs(e , e.fieldName());
        }
        eb.done();

        *newObj = bb.obj();
        return true;
    }

    Status UpdateDriver::update(const BSONObj& oldObj,
                                const StringData& matchedField,
                                BSONObj* newObj,
                                BSONObj* logOpRec) {
        // TODO: assert that update() is called at most once in a !_multi case.

        mutablebson::Document doc(oldObj);
        FieldRefSet targetFields;

        // Ask each of the mods to type check whether they can operate over the current document
        // and, if so, to change that document accordingly.
        for (vector<ModifierInterface*>::iterator it = _mods.begin(); it != _mods.end(); ++it) {
            ModifierInterface::ExecInfo execInfo;
            Status status = (*it)->prepare(doc.root(), matchedField, &execInfo);
            if (!status.isOK()) {
                return status;
            }

            // Gather which fields this mod is interested on and whether these fields were
            // "taken" by previous mods.  Note that not all mods are multi-field mods. When we
            // see an empty field, we may stop looking for others.
            for (int i = 0; i < ModifierInterface::ExecInfo::MAX_NUM_FIELDS; i++) {
                if (execInfo.fieldRef[i] == 0) {
                    break;
                }

                const FieldRef* other;
                if (!targetFields.insert(execInfo.fieldRef[i], &other)) {
                    return Status(ErrorCodes::ConflictingUpdateOperators,
                                  mongoutils::str::stream()
                                      << "Cannot update '" << other->dottedField()
                                      << "' and '" << execInfo.fieldRef[i]->dottedField()
                                      << "' at the same time");
                }
            }

            if (!execInfo.noOp) {
                Status status = (*it)->apply();
                if (!status.isOK()) {
                    return status;
                }
            }
        }

        // If we require a replication oplog entry for this update, go ahead and generate one.
        if (_logOp && logOpRec) {
            mutablebson::Document logDoc;
            for (vector<ModifierInterface*>::iterator it = _mods.begin(); it != _mods.end(); ++it) {
                Status status = (*it)->log(logDoc.root());
                if (!status.isOK()) {
                    return status;
                }
            }
            *logOpRec = logDoc.getObject();
        }

        *newObj = doc.getObject();
        return Status::OK();
    }

    size_t UpdateDriver::numMods() const {
        return _mods.size();
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

    void UpdateDriver::clear() {
        for (vector<ModifierInterface*>::iterator it = _mods.begin(); it != _mods.end(); ++it) {
            delete *it;
        }
    }

} // namespace mongo
