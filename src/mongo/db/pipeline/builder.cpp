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

#include "pch.h"

#include "db/jsobj.h"
#include "db/pipeline/builder.h"


namespace mongo {

    void BuilderObj::append() {
        pBuilder->appendNull(fieldName);
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

    void BuilderObj::append(string s) {
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

    BuilderObj::BuilderObj(
        BSONObjBuilder *pObjBuilder, string theFieldName):
        pBuilder(pObjBuilder),
        fieldName(theFieldName) {
    }


    void BuilderArray::append() {
        pBuilder->appendNull();
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

    void BuilderArray::append(string s) {
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

    BuilderArray::BuilderArray(
        BSONArrayBuilder *pArrayBuilder):
        pBuilder(pArrayBuilder) {
    }

}
