/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/ops/field_ref.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    FieldRef::~FieldRef() {
        clear();
    }

    void FieldRef::parse(const StringData& dottedField) {
        if (dottedField.size() == 0) {
            return;
        }

        // We guarantee that accesses through getPart() will be valid while 'this' is. So we
        // take a copy. We're going to be "chopping" up the copy into c-strings.
        _fieldBase.reset(new char[dottedField.size()]+1);
        memcpy(_fieldBase.get(), dottedField.data(), dottedField.size());
        _fieldBase[dottedField.size()] = '\0';

        // Separate the field parts using '.' as a delimiter.
        char* beg = _fieldBase.get();
        char* cur = beg;
        char* end = beg + dottedField.size();
        while (true) {
            if (cur != end && *cur != '.') {
                cur++;
                continue;
            }

            appendPart(StringData(beg, cur - beg));

            if (cur != end) {
                *cur = '\0';
                beg = ++cur;
                continue;
            }

            break;
        }
    }

    void FieldRef::setPart(size_t i, const StringData& part) {
        dassert(i < _size);

        // If the part was replaced before, we can dispose of its memory.
        StringData currPart = getPart(i);
        PartsSet::iterator it = _replacements.find(currPart.data());
        if (it != _replacements.end()) {
            delete [] *it;
            _replacements.erase(it);
        }

        char* repl = new char[part.size()+1];
        memcpy(repl, part.data(), part.size());
        repl[part.size()] = '\0';
        _replacements.insert(repl);
        if (i < kReserveAhead) {
            _fixed[i] = PartRef(repl, part.size());
        }
        else {
            _variable[getIndex(i)] = PartRef(repl, part.size());
        }
    }

    size_t FieldRef::appendPart(const StringData& part) {
        if (_size < kReserveAhead) {
            _fixed[_size] = PartRef(part.data(), part.size());
        }
        else {
            _variable.push_back(PartRef(part.data(), part.size()));
        }
        return ++_size;
    }

    StringData FieldRef::getPart(size_t i) const {
        dassert(i < _size);

        if (i < kReserveAhead) {
            return StringData(_fixed[i].data, _fixed[i].len);
        }
        else {
            return StringData(_variable[getIndex(i)].data, _variable[getIndex(i)].len);
        }
    }

    void FieldRef::clear() {
        _size = 0;
        _variable.clear();
        _fieldBase.reset();
        for (PartsSet::iterator it = _replacements.begin(); it != _replacements.end(); ++it) {
            delete [] *it;
        }
        _replacements.clear();
    }

    std::string FieldRef::dottedField() const {
        std::string res;
        if (_size == 0) {
            return res;
        }

        res.append(_fixed[0].data, _fixed[0].len);
        for (size_t i=1; i<_size; i++) {
            res.append(1, '.');
            StringData part = getPart(i);
            res.append(part.data(), part.size());
        }
        return res;
    }

} // namespace mongo
