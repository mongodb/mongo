/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/builder.h"


namespace mongo {

    void BuilderObj::append() {
        pBuilder->appendNull(fieldName);
    }

    void BuilderObj::appendUndefined() {
        pBuilder->appendUndefined(fieldName);
    }

    void BuilderObj::append(bool b) {
        pBuilder->append(fieldName, b);
    }

    void BuilderObj::append(int i) {
        pBuilder->append(fieldName, i);
    }

    void BuilderObj::append(long long ll) {
        pBuilder->append(fieldName, ll);
    }

    void BuilderObj::append(double d) {
        pBuilder->append(fieldName, d);
    }

    void BuilderObj::append(StringData s) {
        pBuilder->append(fieldName, s);
    }

    void BuilderObj::append(const OID &o) {
        pBuilder->append(fieldName, o);
    }

    void BuilderObj::append(const Date_t &d) {
        pBuilder->append(fieldName, d);
    }

    void BuilderObj::append(BSONObjBuilder *pDone) {
        pBuilder->append(fieldName, pDone->done());
    }

    void BuilderObj::append(BSONArrayBuilder *pDone) {
        pBuilder->append(fieldName, pDone->arr());
    }

    void BuilderObj::append(const OpTime& ot) {
        pBuilder->appendTimestamp(fieldName, ot.getSecs(), ot.getInc());
    }

    BuilderObj::BuilderObj(BSONObjBuilder *pObjBuilder, StringData theFieldName):
        pBuilder(pObjBuilder),
        fieldName(theFieldName) {
    }


    void BuilderArray::append() {
        pBuilder->appendNull();
    }

    void BuilderArray::appendUndefined() {
        pBuilder->appendUndefined();
    }

    void BuilderArray::append(bool b) {
        pBuilder->append(b);
    }

    void BuilderArray::append(int i) {
        pBuilder->append(i);
    }

    void BuilderArray::append(long long ll) {
        pBuilder->append(ll);
    }

    void BuilderArray::append(double d) {
        pBuilder->append(d);
    }

    void BuilderArray::append(StringData s) {
        pBuilder->append(s);
    }

    void BuilderArray::append(const OID &o) {
        pBuilder->append(o);
    }

    void BuilderArray::append(const Date_t &d) {
        pBuilder->append(d);
    }

    void BuilderArray::append(BSONObjBuilder *pDone) {
        pBuilder->append(pDone->done());
    }

    void BuilderArray::append(BSONArrayBuilder *pDone) {
        pBuilder->append(pDone->arr());
    }

    void BuilderArray::append(const OpTime& ot) {
        pBuilder->appendTimestamp(ot.getSecs(), ot.getInc());
    }

    BuilderArray::BuilderArray(
        BSONArrayBuilder *pArrayBuilder):
        pBuilder(pArrayBuilder) {
    }

}
