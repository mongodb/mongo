// namespacestring-inl.h


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

namespace mongo {

inline StringData NamespaceString::db() const {
    return _dotIndex == std::string::npos ? StringData() : StringData(_ns.c_str(), _dotIndex);
}

inline StringData NamespaceString::coll() const {
    return _dotIndex == std::string::npos ? StringData() : StringData(_ns.c_str() + _dotIndex + 1,
                                                                      _ns.size() - 1 - _dotIndex);
}

inline bool NamespaceString::normal(StringData ns) {
    return !virtualized(ns);
}

inline bool NamespaceString::oplog(StringData ns) {
    return ns.startsWith("local.oplog.");
}

inline bool NamespaceString::special(StringData ns) {
    return !normal(ns) || ns.substr(ns.find('.')).startsWith(".system.");
}

inline bool NamespaceString::virtualized(StringData ns) {
    return ns.find('$') != std::string::npos && ns != "local.oplog.$main";
}

inline bool NamespaceString::validDBName(StringData db, DollarInDbNameBehavior behavior) {
    if (db.size() == 0 || db.size() > 64)
        return false;

    for (StringData::const_iterator iter = db.begin(), end = db.end(); iter != end; ++iter) {
        switch (*iter) {
            case '\0':
            case '/':
            case '\\':
            case '.':
            case ' ':
            case '"':
                return false;
            case '$':
                if (behavior == DollarInDbNameBehavior::Disallow)
                    return false;
                continue;
#ifdef _WIN32
            // We prohibit all FAT32-disallowed characters on Windows
            case '*':
            case '<':
            case '>':
            case ':':
            case '|':
            case '?':
                return false;
#endif
            default:
                continue;
        }
    }
    return true;
}

inline bool NamespaceString::validCollectionComponent(StringData ns) {
    size_t idx = ns.find('.');
    if (idx == std::string::npos)
        return false;

    return validCollectionName(ns.substr(idx + 1)) || oplog(ns);
}

inline bool NamespaceString::validCollectionName(StringData coll) {
    if (coll.empty())
        return false;

    if (coll[0] == '.')
        return false;

    for (StringData::const_iterator iter = coll.begin(), end = coll.end(); iter != end; ++iter) {
        switch (*iter) {
            case '\0':
            case '$':
                return false;
            default:
                continue;
        }
    }

    return true;
}

inline NamespaceString::NamespaceString() : _ns(), _dotIndex(0) {}
inline NamespaceString::NamespaceString(StringData nsIn) {
    _ns = nsIn.toString();  // copy to our buffer
    _dotIndex = _ns.find('.');
}

inline NamespaceString::NamespaceString(StringData dbName, StringData collectionName)
    : _ns(dbName.size() + collectionName.size() + 1, '\0') {
    uassert(17235,
            "'.' is an invalid character in a database name",
            dbName.find('.') == std::string::npos);
    uassert(17246,
            "Collection names cannot start with '.'",
            collectionName.empty() || collectionName[0] != '.');
    std::string::iterator it = std::copy(dbName.begin(), dbName.end(), _ns.begin());
    *it = '.';
    ++it;
    it = std::copy(collectionName.begin(), collectionName.end(), it);
    _dotIndex = dbName.size();
    dassert(it == _ns.end());
    dassert(_ns[_dotIndex] == '.');
    uassert(17295,
            "namespaces cannot have embedded null characters",
            _ns.find('\0') == std::string::npos);
}

inline int nsDBHash(const std::string& ns) {
    int hash = 7;
    for (size_t i = 0; i < ns.size(); i++) {
        if (ns[i] == '.')
            break;
        hash += 11 * (ns[i]);
        hash *= 3;
    }
    return hash;
}

inline bool nsDBEquals(const std::string& a, const std::string& b) {
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] == '.') {
            // b has to either be done or a '.'

            if (b.size() == i)
                return true;

            if (b[i] == '.')
                return true;

            return false;
        }

        // a is another character
        if (b.size() == i)
            return false;

        if (b[i] != a[i])
            return false;
    }

    // a is done
    // make sure b is done
    if (b.size() == a.size() || b[a.size()] == '.')
        return true;

    return false;
}

/* future : this doesn't need to be an inline. */
inline std::string NamespaceString::getSisterNS(StringData local) const {
    verify(local.size() && local[0] != '.');
    return db().toString() + "." + local.toString();
}

inline std::string NamespaceString::getSystemIndexesCollection() const {
    return db().toString() + ".system.indexes";
}

inline std::string NamespaceString::getCommandNS() const {
    return db().toString() + ".$cmd";
}
}
