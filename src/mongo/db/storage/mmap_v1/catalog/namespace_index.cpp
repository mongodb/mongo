// namespace_index.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/catalog/namespace_index.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/catalog/hashtab.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/util/exit.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;
using std::list;
using std::string;

NamespaceIndex::NamespaceIndex(const std::string& dir, const std::string& database)
    : _dir(dir), _database(database), _ht(nullptr) {}

NamespaceIndex::~NamespaceIndex() {}

NamespaceDetails* NamespaceIndex::details(StringData ns) const {
    const Namespace n(ns);
    return details(n);
}

NamespaceDetails* NamespaceIndex::details(const Namespace& ns) const {
    return _ht->get(ns);
}

void NamespaceIndex::add_ns(OperationContext* txn, StringData ns, const DiskLoc& loc, bool capped) {
    NamespaceDetails details(loc, capped);
    add_ns(txn, ns, &details);
}

void NamespaceIndex::add_ns(OperationContext* txn, StringData ns, const NamespaceDetails* details) {
    Namespace n(ns);
    add_ns(txn, n, details);
}

void NamespaceIndex::add_ns(OperationContext* txn,
                            const Namespace& ns,
                            const NamespaceDetails* details) {
    const NamespaceString nss(ns.toString());
    invariant(txn->lockState()->isDbLockedForMode(nss.db(), MODE_X));

    massert(17315, "no . in ns", nsIsFull(nss.toString()));

    uassert(10081, "too many namespaces/collections", _ht->put(txn, ns, *details));
}

void NamespaceIndex::kill_ns(OperationContext* txn, StringData ns) {
    const NamespaceString nss(ns.toString());
    invariant(txn->lockState()->isDbLockedForMode(nss.db(), MODE_X));

    const Namespace n(ns);
    _ht->kill(txn, n);

    if (ns.size() <= Namespace::MaxNsColletionLen) {
        // Larger namespace names don't have room for $extras so they can't exist. The code
        // below would cause an "$extra: ns too large" error and stacktrace to be printed to the
        // log even though everything is fine.
        for (int i = 0; i <= 1; i++) {
            try {
                Namespace extra(n.extraName(i));
                _ht->kill(txn, extra);
            } catch (DBException&) {
                LOG(3) << "caught exception in kill_ns" << endl;
            }
        }
    }
}

bool NamespaceIndex::pathExists() const {
    return boost::filesystem::exists(path());
}

boost::filesystem::path NamespaceIndex::path() const {
    boost::filesystem::path ret(_dir);
    if (storageGlobalParams.directoryperdb)
        ret /= _database;
    ret /= (_database + ".ns");
    return ret;
}

static void namespaceGetNamespacesCallback(const Namespace& k,
                                           NamespaceDetails& v,
                                           list<string>* l) {
    if (!k.hasDollarSign() || k == "local.oplog.$main") {
        // we call out local.oplog.$main specifically as its the only "normal"
        // collection that has a $, so we make sure it gets added
        l->push_back(k.toString());
    }
}

void NamespaceIndex::getCollectionNamespaces(list<string>* tofill) const {
    _ht->iterAll(stdx::bind(
        namespaceGetNamespacesCallback, stdx::placeholders::_1, stdx::placeholders::_2, tofill));
}

void NamespaceIndex::maybeMkdir() const {
    if (!storageGlobalParams.directoryperdb)
        return;
    boost::filesystem::path dir(_dir);
    dir /= _database;
    if (!boost::filesystem::exists(dir))
        MONGO_ASSERT_ON_EXCEPTION_WITH_MSG(boost::filesystem::create_directory(dir),
                                           "create dir for db ");
}

void NamespaceIndex::init(OperationContext* txn) {
    invariant(!_ht.get());

    unsigned long long len = 0;

    const boost::filesystem::path nsPath = path();
    const std::string pathString = nsPath.string();

    void* p = 0;

    if (boost::filesystem::exists(nsPath)) {
        if (_f.open(pathString)) {
            len = _f.length();

            if (len % (1024 * 1024) != 0) {
                StringBuilder sb;
                sb << "Invalid length: " << len << " for .ns file: " << pathString
                   << ". Cannot open database";

                log() << sb.str();
                uassert(10079, sb.str(), len % (1024 * 1024) == 0);
            }

            p = _f.getView();
        }
    } else {
        uassert(ErrorCodes::IllegalOperation,
                "Cannot create a database in read-only mode.",
                !storageGlobalParams.readOnly);

        // use mmapv1GlobalOptions.lenForNewNsFiles, we are making a new database
        massert(10343,
                "bad mmapv1GlobalOptions.lenForNewNsFiles",
                mmapv1GlobalOptions.lenForNewNsFiles >= 1024 * 1024);

        maybeMkdir();

        unsigned long long l = mmapv1GlobalOptions.lenForNewNsFiles;
        log() << "allocating new ns file " << pathString << ", filling with zeroes..." << endl;

        {
            // Due to SERVER-15369 we need to explicitly write zero-bytes to the NS file.
            const unsigned long long kBlockSize = 1024 * 1024;
            invariant(l % kBlockSize == 0);  // ns files can only be multiples of 1MB
            const std::vector<char> zeros(kBlockSize, 0);

            File file;
            file.open(pathString.c_str());

            massert(18825, str::stream() << "couldn't create file " << pathString, file.is_open());

            for (fileofs ofs = 0; ofs < l && !file.bad(); ofs += kBlockSize) {
                file.write(ofs, &zeros[0], kBlockSize);
            }

            if (file.bad()) {
                try {
                    boost::filesystem::remove(pathString);
                } catch (const std::exception& e) {
                    StringBuilder ss;
                    ss << "error removing file: " << e.what();
                    massert(18909, ss.str(), 0);
                }
            } else {
                file.fsync();
            }

            massert(18826, str::stream() << "failure writing file " << pathString, !file.bad());
        }

        if (_f.create(pathString, l)) {
            // The writes done in this function must not be rolled back. This will leave the
            // file empty, but available for future use. That is why we go directly to the
            // global dur dirty list rather than going through the OperationContext.
            getDur().createdFile(pathString, l);

            // Commit the journal and all changes to disk so that even if exceptions occur
            // during subsequent initialization, we won't have uncommited changes during file
            // close.
            getDur().commitNow(txn);

            len = l;
            invariant(len == mmapv1GlobalOptions.lenForNewNsFiles);

            p = _f.getView();
        }
    }

    if (p == 0) {
        severe() << "error couldn't open file " << pathString << " terminating" << endl;
        invariant(false);
    }

    invariant(len <= 0x7fffffff);
    _ht.reset(new NamespaceHashTable(p, (int)len, "namespace index"));
}
}
