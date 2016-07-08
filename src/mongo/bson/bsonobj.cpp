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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/db/jsobj.h"

#include <boost/functional/hash.hpp>

#include "mongo/base/data_range.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/db/json.h"
#include "mongo/util/allocator.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"

namespace mongo {
using namespace std;
/* BSONObj ------------------------------------------------------------*/

// deep (full) equality
bool BSONObj::equal(const BSONObj& rhs) const {
    BSONObjIterator i(*this);
    BSONObjIterator j(rhs);
    BSONElement l, r;
    do {
        // so far, equal...
        l = i.next();
        r = j.next();
        if (l.eoo())
            return r.eoo();
    } while (l == r);
    return false;
}

void BSONObj::_assertInvalid() const {
    StringBuilder ss;
    int os = objsize();
    ss << "BSONObj size: " << os << " (0x" << integerToHex(os) << ") is invalid. "
       << "Size must be between 0 and " << BSONObjMaxInternalSize << "("
       << (BSONObjMaxInternalSize / (1024 * 1024)) << "MB)";
    try {
        BSONElement e = firstElement();
        ss << " First element: " << e.toString();
    } catch (...) {
    }
    massert(10334, ss.str(), 0);
}

BSONObj BSONObj::copy() const {
    auto storage = SharedBuffer::allocate(objsize());
    memcpy(storage.get(), objdata(), objsize());
    return BSONObj(std::move(storage));
}

BSONObj BSONObj::getOwned() const {
    if (isOwned())
        return *this;
    return copy();
}

string BSONObj::jsonString(JsonStringFormat format, int pretty, bool isArray) const {
    if (isEmpty())
        return isArray ? "[]" : "{}";

    StringBuilder s;
    s << (isArray ? "[ " : "{ ");
    BSONObjIterator i(*this);
    BSONElement e = i.next();
    if (!e.eoo())
        while (1) {
            s << e.jsonString(format, !isArray, pretty ? pretty + 1 : 0);
            e = i.next();
            if (e.eoo())
                break;
            s << ",";
            if (pretty) {
                s << '\n';
                for (int x = 0; x < pretty; x++)
                    s << "  ";
            } else {
                s << " ";
            }
        }
    s << (isArray ? " ]" : " }");
    return s.str();
}

bool BSONObj::valid() const {
    return validateBSON(objdata(), objsize()).isOK();
}

int BSONObj::woCompare(const BSONObj& r,
                       const Ordering& o,
                       bool considerFieldName,
                       const StringData::ComparatorInterface* comparator) const {
    if (isEmpty())
        return r.isEmpty() ? 0 : -1;
    if (r.isEmpty())
        return 1;

    BSONObjIterator i(*this);
    BSONObjIterator j(r);
    unsigned mask = 1;
    while (1) {
        // so far, equal...

        BSONElement l = i.next();
        BSONElement r = j.next();
        if (l.eoo())
            return r.eoo() ? 0 : -1;
        if (r.eoo())
            return 1;

        int x;
        {
            x = l.woCompare(r, considerFieldName, comparator);
            if (o.descending(mask))
                x = -x;
        }
        if (x != 0)
            return x;
        mask <<= 1;
    }
    return -1;
}

/* well ordered compare */
int BSONObj::woCompare(const BSONObj& r,
                       const BSONObj& idxKey,
                       bool considerFieldName,
                       const StringData::ComparatorInterface* comparator) const {
    if (isEmpty())
        return r.isEmpty() ? 0 : -1;
    if (r.isEmpty())
        return 1;

    bool ordered = !idxKey.isEmpty();

    BSONObjIterator i(*this);
    BSONObjIterator j(r);
    BSONObjIterator k(idxKey);
    while (1) {
        // so far, equal...

        BSONElement l = i.next();
        BSONElement r = j.next();
        BSONElement o;
        if (ordered)
            o = k.next();
        if (l.eoo())
            return r.eoo() ? 0 : -1;
        if (r.eoo())
            return 1;

        int x;
        /*
                    if( ordered && o.type() == String && strcmp(o.valuestr(), "ascii-proto") == 0 &&
                        l.type() == String && r.type() == String ) {
                        // note: no negative support yet, as this is just sort of a POC
                        x = _stricmp(l.valuestr(), r.valuestr());
                    }
                    else*/ {
            x = l.woCompare(r, considerFieldName, comparator);
            if (ordered && o.number() < 0)
                x = -x;
        }
        if (x != 0)
            return x;
    }
    return -1;
}

size_t BSONObj::Hasher::operator()(const BSONObj& obj) const {
    size_t hash = 0;
    BSONForEach(elem, obj) {
        boost::hash_combine(hash, BSONElement::Hasher()(elem));
    }
    return hash;
}

bool BSONObj::isPrefixOf(const BSONObj& otherObj) const {
    BSONObjIterator a(*this);
    BSONObjIterator b(otherObj);

    while (a.more() && b.more()) {
        BSONElement x = a.next();
        BSONElement y = b.next();
        if (x != y)
            return false;
    }

    return !a.more();
}

bool BSONObj::isFieldNamePrefixOf(const BSONObj& otherObj) const {
    BSONObjIterator a(*this);
    BSONObjIterator b(otherObj);

    while (a.more() && b.more()) {
        BSONElement x = a.next();
        BSONElement y = b.next();
        if (!str::equals(x.fieldName(), y.fieldName())) {
            return false;
        }
    }

    return !a.more();
}

BSONObj BSONObj::extractFieldsUnDotted(const BSONObj& pattern) const {
    BSONObjBuilder b;
    BSONObjIterator i(pattern);
    while (i.moreWithEOO()) {
        BSONElement e = i.next();
        if (e.eoo())
            break;
        BSONElement x = getField(e.fieldName());
        if (!x.eoo())
            b.appendAs(x, "");
    }
    return b.obj();
}

BSONObj BSONObj::filterFieldsUndotted(const BSONObj& filter, bool inFilter) const {
    BSONObjBuilder b;
    BSONObjIterator i(*this);
    while (i.moreWithEOO()) {
        BSONElement e = i.next();
        if (e.eoo())
            break;
        BSONElement x = filter.getField(e.fieldName());
        if ((x.eoo() && !inFilter) || (!x.eoo() && inFilter))
            b.append(e);
    }
    return b.obj();
}

BSONElement BSONObj::getFieldUsingIndexNames(StringData fieldName, const BSONObj& indexKey) const {
    BSONObjIterator i(indexKey);
    int j = 0;
    while (i.moreWithEOO()) {
        BSONElement f = i.next();
        if (f.eoo())
            return BSONElement();
        if (f.fieldName() == fieldName)
            break;
        ++j;
    }
    BSONObjIterator k(*this);
    while (k.moreWithEOO()) {
        BSONElement g = k.next();
        if (g.eoo())
            return BSONElement();
        if (j == 0) {
            return g;
        }
        --j;
    }
    return BSONElement();
}

/* grab names of all the fields in this object */
int BSONObj::getFieldNames(set<string>& fields) const {
    int n = 0;
    BSONObjIterator i(*this);
    while (i.moreWithEOO()) {
        BSONElement e = i.next();
        if (e.eoo())
            break;
        fields.insert(e.fieldName());
        n++;
    }
    return n;
}

/* note: addFields always adds _id even if not specified
   returns n added not counting _id unless requested.
*/
int BSONObj::addFields(BSONObj& from, set<string>& fields) {
    verify(isEmpty() && !isOwned()); /* partial implementation for now... */

    BSONObjBuilder b;

    int N = fields.size();
    int n = 0;
    BSONObjIterator i(from);
    bool gotId = false;
    while (i.moreWithEOO()) {
        BSONElement e = i.next();
        const char* fname = e.fieldName();
        if (fields.count(fname)) {
            b.append(e);
            ++n;
            gotId = gotId || strcmp(fname, "_id") == 0;
            if (n == N && gotId)
                break;
        } else if (strcmp(fname, "_id") == 0) {
            b.append(e);
            gotId = true;
            if (n == N && gotId)
                break;
        }
    }

    if (n) {
        *this = b.obj();
    }

    return n;
}

bool BSONObj::couldBeArray() const {
    BSONObjIterator i(*this);
    int index = 0;
    while (i.moreWithEOO()) {
        BSONElement e = i.next();
        if (e.eoo())
            break;

        // TODO:  If actually important, may be able to do int->char* much faster
        if (strcmp(e.fieldName(), ((string)(str::stream() << index)).c_str()) != 0)
            return false;
        index++;
    }
    return true;
}

BSONObj BSONObj::clientReadable() const {
    BSONObjBuilder b;
    BSONObjIterator i(*this);
    while (i.moreWithEOO()) {
        BSONElement e = i.next();
        if (e.eoo())
            break;
        switch (e.type()) {
            case MinKey: {
                BSONObjBuilder m;
                m.append("$minElement", 1);
                b.append(e.fieldName(), m.done());
                break;
            }
            case MaxKey: {
                BSONObjBuilder m;
                m.append("$maxElement", 1);
                b.append(e.fieldName(), m.done());
                break;
            }
            default:
                b.append(e);
        }
    }
    return b.obj();
}

BSONObj BSONObj::replaceFieldNames(const BSONObj& names) const {
    BSONObjBuilder b;
    BSONObjIterator i(*this);
    BSONObjIterator j(names);
    BSONElement f = j.moreWithEOO() ? j.next() : BSONObj().firstElement();
    while (i.moreWithEOO()) {
        BSONElement e = i.next();
        if (e.eoo())
            break;
        if (!f.eoo()) {
            b.appendAs(e, f.fieldName());
            f = j.next();
        } else {
            b.append(e);
        }
    }
    return b.obj();
}

Status BSONObj::_okForStorage(bool root, bool deep) const {
    BSONObjIterator i(*this);

    // The first field is special in the case of a DBRef where the first field must be $ref
    bool first = true;
    while (i.more()) {
        BSONElement e = i.next();
        const char* name = e.fieldName();

        // Cannot start with "$", unless dbref which must start with ($ref, $id)
        if (str::startsWith(name, '$')) {
            if (first &&
                // $ref is a collection name and must be a String
                str::equals(name, "$ref") &&
                e.type() == String && str::equals(i.next().fieldName(), "$id")) {
                first = false;
                // keep inspecting fields for optional "$db"
                e = i.next();
                name = e.fieldName();  // "" if eoo()

                // optional $db field must be a String
                if (str::equals(name, "$db") && e.type() == String) {
                    continue;  // this element is fine, so continue on to siblings (if any more)
                }

                // Can't start with a "$", all other checks are done below (outside if blocks)
                if (str::startsWith(name, '$')) {
                    return Status(ErrorCodes::DollarPrefixedFieldName,
                                  str::stream() << name << " is not valid for storage.");
                }
            } else {
                // not an okay, $ prefixed field name.
                return Status(ErrorCodes::DollarPrefixedFieldName,
                              str::stream() << name << " is not valid for storage.");
            }
        }

        // Do not allow "." in the field name
        if (strchr(name, '.')) {
            return Status(ErrorCodes::DottedFieldName,
                          str::stream() << name << " is not valid for storage.");
        }

        // (SERVER-9502) Do not allow storing an _id field with a RegEx type or
        // Array type in a root document
        if (root && (e.type() == RegEx || e.type() == Array || e.type() == Undefined) &&
            str::equals(name, "_id")) {
            return Status(ErrorCodes::InvalidIdField,
                          str::stream() << name
                                        << " is not valid for storage because it is of type "
                                        << typeName(e.type()));
        }

        if (deep && e.mayEncapsulate()) {
            switch (e.type()) {
                case Object:
                case Array: {
                    Status s = e.embeddedObject()._okForStorage(false, true);
                    // TODO: combine field names for better error messages
                    if (!s.isOK())
                        return s;
                } break;
                case CodeWScope: {
                    Status s = e.codeWScopeObject()._okForStorage(false, true);
                    // TODO: combine field names for better error messages
                    if (!s.isOK())
                        return s;
                } break;
                default:
                    uassert(12579, "unhandled cases in BSONObj okForStorage", 0);
            }
        }

        // After we have processed one field, we are no longer on the first field
        first = false;
    }
    return Status::OK();
}

void BSONObj::dump() const {
    LogstreamBuilder builder = log();
    builder << hex;
    const char* p = objdata();
    for (int i = 0; i < objsize(); i++) {
        builder << i << '\t' << (0xff & ((unsigned)*p));
        if (*p >= 'A' && *p <= 'z')
            builder << '\t' << *p;
        builder << endl;
        p++;
    }
}

void BSONObj::getFields(unsigned n, const char** fieldNames, BSONElement* fields) const {
    BSONObjIterator i(*this);
    while (i.more()) {
        BSONElement e = i.next();
        const char* p = e.fieldName();
        for (unsigned i = 0; i < n; i++) {
            if (strcmp(p, fieldNames[i]) == 0) {
                fields[i] = e;
                break;
            }
        }
    }
}

BSONElement BSONObj::getField(StringData name) const {
    BSONObjIterator i(*this);
    while (i.more()) {
        BSONElement e = i.next();
        // We know that e has a cached field length since BSONObjIterator::next internally
        // called BSONElement::size on the BSONElement that it returned, so it is more
        // efficient to re-use that information by obtaining the field name as a
        // StringData, which will be pre-populated with the cached length.
        if (name == e.fieldNameStringData())
            return e;
    }
    return BSONElement();
}

int BSONObj::getIntField(StringData name) const {
    BSONElement e = getField(name);
    return e.isNumber() ? (int)e.number() : std::numeric_limits<int>::min();
}

bool BSONObj::getBoolField(StringData name) const {
    BSONElement e = getField(name);
    return e.type() == Bool ? e.boolean() : false;
}

const char* BSONObj::getStringField(StringData name) const {
    BSONElement e = getField(name);
    return e.type() == String ? e.valuestr() : "";
}

bool BSONObj::getObjectID(BSONElement& e) const {
    BSONElement f = getField("_id");
    if (!f.eoo()) {
        e = f;
        return true;
    }
    return false;
}

BSONObj BSONObj::removeField(StringData name) const {
    BSONObjBuilder b;
    BSONObjIterator i(*this);
    while (i.more()) {
        BSONElement e = i.next();
        const char* fname = e.fieldName();
        if (name != fname)
            b.append(e);
    }
    return b.obj();
}

std::string BSONObj::hexDump() const {
    std::stringstream ss;
    const char* d = objdata();
    int size = objsize();
    for (int i = 0; i < size; ++i) {
        ss.width(2);
        ss.fill('0');
        ss << std::hex << (unsigned)(unsigned char)(d[i]) << std::dec;
        if ((d[i] >= '0' && d[i] <= '9') || (d[i] >= 'A' && d[i] <= 'z'))
            ss << '\'' << d[i] << '\'';
        if (i != size - 1)
            ss << ' ';
    }
    return ss.str();
}


void BSONObj::elems(std::vector<BSONElement>& v) const {
    BSONObjIterator i(*this);
    while (i.more())
        v.push_back(i.next());
}

void BSONObj::elems(std::list<BSONElement>& v) const {
    BSONObjIterator i(*this);
    while (i.more())
        v.push_back(i.next());
}

BSONObj BSONObj::getObjectField(StringData name) const {
    BSONElement e = getField(name);
    BSONType t = e.type();
    return t == Object || t == Array ? e.embeddedObject() : BSONObj();
}

int BSONObj::nFields() const {
    int n = 0;
    BSONObjIterator i(*this);
    while (i.moreWithEOO()) {
        BSONElement e = i.next();
        if (e.eoo())
            break;
        n++;
    }
    return n;
}

std::string BSONObj::toString(bool redactValues) const {
    if (isEmpty())
        return "{}";
    StringBuilder s;
    toString(s, false, false, redactValues);
    return s.str();
}
void BSONObj::toString(
    StringBuilder& s, bool isArray, bool full, bool redactValues, int depth) const {
    if (isEmpty()) {
        s << (isArray ? "[]" : "{}");
        return;
    }

    s << (isArray ? "[ " : "{ ");
    BSONObjIterator i(*this);
    bool first = true;
    while (1) {
        massert(10327, "Object does not end with EOO", i.moreWithEOO());
        BSONElement e = i.next(true);
        massert(10328, "Invalid element size", e.size() > 0);
        massert(10329, "Element too large", e.size() < (1 << 30));
        int offset = (int)(e.rawdata() - this->objdata());
        massert(10330, "Element extends past end of object", e.size() + offset <= this->objsize());
        bool end = (e.size() + offset == this->objsize());
        if (e.eoo()) {
            massert(10331, "EOO Before end of object", end);
            break;
        }
        if (first)
            first = false;
        else
            s << ", ";
        e.toString(s, !isArray, full, redactValues, depth);
    }
    s << (isArray ? " ]" : " }");
}

Status DataType::Handler<BSONObj>::store(
    const BSONObj& bson, char* ptr, size_t length, size_t* advanced, std::ptrdiff_t debug_offset) {
    if (bson.objsize() > static_cast<int>(length)) {
        mongoutils::str::stream ss;
        ss << "buffer too small to write bson of size (" << bson.objsize()
           << ") at offset: " << debug_offset;
        return Status(ErrorCodes::Overflow, ss);
    }

    if (ptr) {
        std::memcpy(ptr, bson.objdata(), bson.objsize());
    }

    if (advanced) {
        *advanced = bson.objsize();
    }

    return Status::OK();
}

std::ostream& operator<<(std::ostream& s, const BSONObj& o) {
    return s << o.toString();
}

StringBuilder& operator<<(StringBuilder& s, const BSONObj& o) {
    o.toString(s);
    return s;
}

/** Compare two bson elements, provided as const char *'s, by field name. */
class BSONIteratorSorted::ElementFieldCmp {
public:
    ElementFieldCmp(bool isArray);
    bool operator()(const char* s1, const char* s2) const;

private:
    LexNumCmp _cmp;
};

BSONIteratorSorted::ElementFieldCmp::ElementFieldCmp(bool isArray) : _cmp(!isArray) {}

bool BSONIteratorSorted::ElementFieldCmp::operator()(const char* s1, const char* s2) const {
    // Skip the type byte and compare field names.
    return _cmp(s1 + 1, s2 + 1);
}

BSONIteratorSorted::BSONIteratorSorted(const BSONObj& o, const ElementFieldCmp& cmp)
    : _nfields(o.nFields()), _fields(new const char*[_nfields]) {
    int x = 0;
    BSONObjIterator i(o);
    while (i.more()) {
        _fields[x++] = i.next().rawdata();
        verify(_fields[x - 1]);
    }
    verify(x == _nfields);
    std::sort(_fields.get(), _fields.get() + _nfields, cmp);
    _cur = 0;
}

BSONObjIteratorSorted::BSONObjIteratorSorted(const BSONObj& object)
    : BSONIteratorSorted(object, ElementFieldCmp(false)) {}

BSONArrayIteratorSorted::BSONArrayIteratorSorted(const BSONArray& array)
    : BSONIteratorSorted(array, ElementFieldCmp(true)) {}

}  // namespace mongo
