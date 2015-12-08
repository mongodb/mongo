/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/db/jsobj.h"

namespace mongo {

int getGtLtOp(const BSONElement& e) {
    if (e.type() != Object)
        return BSONObj::Equality;

    BSONElement fe = e.embeddedObject().firstElement();
    return fe.getGtLtOp();
}

bool fieldsMatch(const BSONObj& lhs, const BSONObj& rhs) {
    BSONObjIterator l(lhs);
    BSONObjIterator r(rhs);

    while (l.more() && r.more()) {
        if (strcmp(l.next().fieldName(), r.next().fieldName())) {
            return false;
        }
    }

    return !(l.more() || r.more());  // false if lhs and rhs have diff nFields()
}

Labeler::Label GT("$gt");
Labeler::Label GTE("$gte");
Labeler::Label LT("$lt");
Labeler::Label LTE("$lte");
Labeler::Label NE("$ne");
Labeler::Label NIN("$nin");
Labeler::Label BSIZE("$size");

GENOIDLabeler GENOID;
DateNowLabeler DATENOW;
NullLabeler BSONNULL;
UndefinedLabeler BSONUndefined;

MinKeyLabeler MINKEY;
MaxKeyLabeler MAXKEY;

BSONObjBuilderValueStream::BSONObjBuilderValueStream(BSONObjBuilder* builder) {
    _builder = builder;
}

void BSONObjBuilderValueStream::reset() {
    _fieldName = StringData();
    _subobj.reset();
}

BSONObjBuilder& BSONObjBuilderValueStream::operator<<(const BSONElement& e) {
    _builder->appendAs(e, _fieldName);
    _fieldName = StringData();
    return *_builder;
}

BufBuilder& BSONObjBuilderValueStream::subobjStart() {
    StringData tmp = _fieldName;
    _fieldName = StringData();
    return _builder->subobjStart(tmp);
}

BufBuilder& BSONObjBuilderValueStream::subarrayStart() {
    StringData tmp = _fieldName;
    _fieldName = StringData();
    return _builder->subarrayStart(tmp);
}

Labeler BSONObjBuilderValueStream::operator<<(const Labeler::Label& l) {
    return Labeler(l, this);
}

void BSONObjBuilderValueStream::endField(StringData nextFieldName) {
    if (haveSubobj()) {
        verify(_fieldName.rawData());
        _builder->append(_fieldName, subobj()->done());
        _subobj.reset();
    }
    _fieldName = nextFieldName;
}

BSONObjBuilder* BSONObjBuilderValueStream::subobj() {
    if (!haveSubobj())
        _subobj.reset(new BSONObjBuilder());
    return _subobj.get();
}

BSONObjBuilder& Labeler::operator<<(const BSONElement& e) {
    s_->subobj()->appendAs(e, l_.l_);
    return *s_->_builder;
}

}  // namespace mongo
