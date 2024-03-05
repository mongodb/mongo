// collection_catalog_entry.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/assert_util.h"
#include <cstddef>
#include <numeric>

namespace mongo {
namespace {

// An index will fail to get created if the size in bytes of its key pattern is greater than 2048.
// We use that value to represent the largest number of path components we could ever possibly
// expect to see in an indexed field.
const size_t kMaxKeyPatternPathLength = 2048;
char multikeyPathsEncodedAsBytes[kMaxKeyPatternPathLength];

/**
 * Encodes 'multikeyPaths' as binary data and appends it to 'bob'.
 *
 * For example, consider the index {'a.b': 1, 'a.c': 1} where the paths "a" and "a.b" cause it to be
 * multikey. The object {'a.b': HexData('0101'), 'a.c': HexData('0100')} would then be appended to
 * 'bob'.
 */
void appendMultikeyPathsAsBytes(BSONObj keyPattern,
                                const MultikeyPaths& multikeyPaths,
                                BSONObjBuilder* bob) {
    size_t i = 0;
    for (const auto keyElem : keyPattern) {
        StringData keyName = keyElem.fieldNameStringData();
        size_t numParts = FieldRef{keyName}.numParts();
        invariant(numParts > 0);
        invariant(numParts <= kMaxKeyPatternPathLength);

        std::fill_n(multikeyPathsEncodedAsBytes, numParts, 0);
        for (const auto multikeyComponent : multikeyPaths[i]) {
            multikeyPathsEncodedAsBytes[multikeyComponent] = 1;
        }
        bob->appendBinData(keyName, numParts, BinDataGeneral, &multikeyPathsEncodedAsBytes[0]);

        ++i;
    }
}

/**
 * Parses the path-level multikey information encoded as binary data from 'multikeyPathsObj' and
 * sets 'multikeyPaths' as that value.
 *
 * For example, consider the index {'a.b': 1, 'a.c': 1} where the paths "a" and "a.b" cause it to be
 * multikey. The binary data {'a.b': HexData('0101'), 'a.c': HexData('0100')} would then be parsed
 * into std::vector<std::set<size_t>>{{0U, 1U}, {0U}}.
 */
void parseMultikeyPathsFromBytes(BSONObj multikeyPathsObj, MultikeyPaths* multikeyPaths) {
    invariant(multikeyPaths);
    for (auto elem : multikeyPathsObj) {
        std::set<size_t> multikeyComponents;
        int len;
        const char* data = elem.binData(len);
        invariant(len > 0);
        invariant(static_cast<size_t>(len) <= kMaxKeyPatternPathLength);

        for (int i = 0; i < len; ++i) {
            if (data[i]) {
                multikeyComponents.insert(i);
            }
        }
        multikeyPaths->push_back(multikeyComponents);
    }
}

}  // namespace


class Collection;
class IndexDescriptor;
class OperationContext;

class CollectionCatalogEntry {
public:
    CollectionCatalogEntry(StringData ns) : _ns(ns) {}
    virtual ~CollectionCatalogEntry() {}

    const NamespaceString& ns() const {
        return _ns;
    }

    // ------- indexes ----------


    struct IndexMetaData {
        IndexMetaData() = default;
        IndexMetaData(
            BSONObj s, bool r, RecordId h, bool m, KVPrefix prefix, bool isBackgroundSecondaryBuild)
            : spec(s),
              ready(r),
              head(h),
              multikey(m),
              prefix(prefix),
              isBackgroundSecondaryBuild(isBackgroundSecondaryBuild) {}

        void updateTTLSetting(long long newExpireSeconds) {
            BSONObjBuilder b;
            for (BSONObjIterator bi(spec); bi.more();) {
                BSONElement e = bi.next();
                if (e.fieldNameStringData() == "expireAfterSeconds") {
                    continue;
                }
                b.append(e);
            }

            b.append("expireAfterSeconds", newExpireSeconds);
            spec = b.obj();
        }
        std::string name() const {
            return spec["name"].String();
        }

        BSONObj spec;
        bool ready;
        RecordId head;
        bool multikey;
        KVPrefix prefix = KVPrefix::kNotPrefixed;
        bool isBackgroundSecondaryBuild;

        // If non-empty, 'multikeyPaths' is a vector with size equal to the number of elements in
        // the index key pattern. Each element in the vector is an ordered set of positions
        // (starting at 0) into the corresponding indexed field that represent what prefixes of the
        // indexed field cause the index to be multikey.
        MultikeyPaths multikeyPaths;
    };

    struct MetaData {
        void parse(const BSONObj& obj) {
            ns = obj["ns"].valuestrsafe();

            if (obj["options"].isABSONObj()) {
                options.parse(obj["options"].Obj(), CollectionOptions::parseForStorage)
                    .transitional_ignore();
            }

            BSONElement indexList = obj["indexes"];

            if (indexList.isABSONObj()) {
                for (BSONElement elt : indexList.Obj()) {
                    BSONObj idx = elt.Obj();
                    IndexMetaData imd;
                    imd.spec = idx["spec"].Obj().getOwned();
                    imd.ready = idx["ready"].trueValue();
                    if (idx.hasField("head")) {
                        imd.head = RecordId(idx["head"].Long());
                    } else {
                        imd.head = RecordId(idx["head_a"].Int(), idx["head_b"].Int());
                    }
                    imd.multikey = idx["multikey"].trueValue();

                    if (auto multikeyPathsElem = idx["multikeyPaths"]) {
                        parseMultikeyPathsFromBytes(multikeyPathsElem.Obj(), &imd.multikeyPaths);
                    }

                    imd.prefix = KVPrefix::fromBSONElement(idx["prefix"]);
                    auto bgSecondary = BSONElement(idx["backgroundSecondary"]);
                    // Opt-in to rebuilding behavior for old-format index catalog objects.
                    imd.isBackgroundSecondaryBuild = bgSecondary.eoo() || bgSecondary.trueValue();
                    indexes.push_back(std::move(imd));
                }
            }

            prefix = KVPrefix::fromBSONElement(obj["prefix"]);
        }
        BSONObj toBSON() const {
            BSONObjBuilder b;
            b.append("ns", ns);
            b.append("options", options.toBSON());
            {
                BSONArrayBuilder arr(b.subarrayStart("indexes"));
                for (unsigned i = 0; i < indexes.size(); i++) {
                    BSONObjBuilder sub(arr.subobjStart());
                    sub.append("spec", indexes[i].spec);
                    sub.appendBool("ready", indexes[i].ready);
                    sub.appendBool("multikey", indexes[i].multikey);

                    if (!indexes[i].multikeyPaths.empty()) {
                        BSONObjBuilder subMultikeyPaths(sub.subobjStart("multikeyPaths"));
                        appendMultikeyPathsAsBytes(indexes[i].spec.getObjectField("key"),
                                                   indexes[i].multikeyPaths,
                                                   &subMultikeyPaths);
                        subMultikeyPaths.doneFast();
                    }

                    sub.append("head", static_cast<long long>(indexes[i].head.repr()));
                    sub.append("prefix", indexes[i].prefix.toBSONValue());
                    sub.append("backgroundSecondary", indexes[i].isBackgroundSecondaryBuild);
                    sub.doneFast();
                }
                arr.doneFast();
            }
            b.append("prefix", prefix.toBSONValue());
            return b.obj();
        }


        int findIndexOffset(StringData name) const {
            for (size_t i = 0; i < indexes.size(); i++) {
                if (indexes[i].name() == name) {
                    return i;
                }
            }
            return -1;
        }

        BSONObj getIndexSpec(StringData name) const {
            int offset = findIndexOffset(name);
            invariant(offset >= 0);
            return indexes[offset].spec;
        }

        /**
         * Removes information about an index from the MetaData. Returns true if an index
         * called name existed and was deleted, and false otherwise.
         */
        bool eraseIndex(StringData name) {
            int indexOffset = findIndexOffset(name);

            if (indexOffset < 0) {
                return false;
            }

            indexes.erase(indexes.begin() + indexOffset);
            return true;
        }

        void rename(StringData toNS) {
            ns = toNS.toString();
            for (size_t i = 0; i < indexes.size(); i++) {
                BSONObj spec = indexes[i].spec;
                BSONObjBuilder b;
                // Add the fields in the same order they were in the original specification.
                for (auto&& elem : spec) {
                    if (elem.fieldNameStringData() == "ns") {
                        b.append("ns", toNS);
                    } else {
                        b.append(elem);
                    }
                }
                indexes[i].spec = b.obj();
            }
        }

        KVPrefix getMaxPrefix() const {
            // Use the collection prefix as the initial max value seen. Then compare it with each
            // index prefix. Note the oplog has no indexes so the vector of 'IndexMetaData' may be
            // empty.
            return std::accumulate(
                indexes.begin(), indexes.end(), prefix, [](KVPrefix max, IndexMetaData index) {
                    return max < index.prefix ? index.prefix : max;
                });
        }

        std::string ns;
        CollectionOptions options;
        std::vector<IndexMetaData> indexes;
        KVPrefix prefix = KVPrefix::kNotPrefixed;
    };

    virtual MetaData getMetaData(OperationContext* opCtx) const {
        MONGO_UNREACHABLE;
    }

    virtual CollectionOptions getCollectionOptions(OperationContext* opCtx) const = 0;

    virtual int getTotalIndexCount(OperationContext* opCtx) const = 0;

    virtual int getCompletedIndexCount(OperationContext* opCtx) const = 0;

    virtual int getMaxAllowedIndexes() const = 0;

    virtual void getAllIndexes(OperationContext* opCtx, std::vector<std::string>* names) const = 0;

    virtual void getReadyIndexes(OperationContext* opCtx,
                                 std::vector<std::string>* names) const = 0;

    virtual void getAllUniqueIndexes(OperationContext* opCtx,
                                     std::vector<std::string>* names) const {}

    virtual BSONObj getIndexSpec(OperationContext* opCtx, StringData idxName) const = 0;

    /**
     * Returns true if the index identified by 'indexName' is multikey, and returns false otherwise.
     *
     * If the 'multikeyPaths' pointer is non-null, then it must point to an empty vector. If this
     * index supports tracking path-level multikey information, then this function sets
     * 'multikeyPaths' as the path components that cause this index to be multikey.
     *
     * In particular, if this function returns false and the index supports tracking path-level
     * multikey information, then 'multikeyPaths' is initialized as a vector with size equal to the
     * number of elements in the index key pattern of empty sets.
     */
    virtual bool isIndexMultikey(OperationContext* opCtx,
                                 StringData indexName,
                                 MultikeyPaths* multikeyPaths) const = 0;

    /**
     * Sets the index identified by 'indexName' to be multikey.
     *
     * If 'multikeyPaths' is non-empty, then it must be a vector with size equal to the number of
     * elements in the index key pattern. Additionally, at least one path component of the indexed
     * fields must cause this index to be multikey.
     *
     * This function returns true if the index metadata has changed, and returns false otherwise.
     */
    virtual bool setIndexIsMultikey(OperationContext* opCtx,
                                    StringData indexName,
                                    const MultikeyPaths& multikeyPaths) = 0;

    virtual RecordId getIndexHead(OperationContext* opCtx, StringData indexName) const = 0;

    virtual void setIndexHead(OperationContext* opCtx,
                              StringData indexName,
                              const RecordId& newHead) = 0;

    virtual bool isIndexReady(OperationContext* opCtx, StringData indexName) const = 0;

    virtual bool isIndexPresent(OperationContext* opCtx, StringData indexName) const = 0;

    virtual KVPrefix getIndexPrefix(OperationContext* opCtx, StringData indexName) const = 0;

    virtual Status removeIndex(OperationContext* opCtx, StringData indexName) = 0;

    virtual Status prepareForIndexBuild(OperationContext* opCtx,
                                        const IndexDescriptor* spec,
                                        bool isBackgroundSecondaryBuild) = 0;

    virtual void indexBuildSuccess(OperationContext* opCtx, StringData indexName) = 0;

    /* Updates the expireAfterSeconds field of the given index to the value in newExpireSecs.
     * The specified index must already contain an expireAfterSeconds field, and the value in
     * that field and newExpireSecs must both be numeric.
     */
    virtual void updateTTLSetting(OperationContext* opCtx,
                                  StringData idxName,
                                  long long newExpireSeconds) = 0;

    virtual void updateIndexMetadata(OperationContext* opCtx, const IndexDescriptor* desc) {}

    /**
     * Sets the flags field of CollectionOptions to newValue.
     * Subsequent calls to getCollectionOptions should have flags==newValue and flagsSet==true.
     */
    virtual void updateFlags(OperationContext* opCtx, int newValue) = 0;

    /**
     * Updates the validator for this collection.
     *
     * An empty validator removes all validation.
     */
    virtual void updateValidator(OperationContext* opCtx,
                                 const BSONObj& validator,
                                 StringData validationLevel,
                                 StringData validationAction) = 0;

    /**
     * Updates the 'temp' setting for this collection.
     */
    virtual void setIsTemp(OperationContext* opCtx, bool isTemp) = 0;

    /**
     * Assigns a new UUID to this collection. All collections must have UUIDs, so this is called if
     * a collection erroneously does not have a UUID.
     */
    virtual void addUUID(OperationContext* opCtx, CollectionUUID uuid, Collection* coll) = 0;

    /**
     * Compare the UUID argument to the UUID obtained from the metadata. Return true if they
     * are equal, false otherwise. uuid can become a CollectionUUID once MMAPv1 is removed.
     */
    virtual bool isEqualToMetadataUUID(OperationContext* opCtx, OptionalCollectionUUID uuid) = 0;

    /**
     * Updates size of a capped Collection.
     */
    virtual void updateCappedSize(OperationContext* opCtx, long long size) = 0;

    virtual RecordStore* getRecordStore() {
        return nullptr;
    }

    virtual  RecordStore* getRecordStore() const {
        return nullptr;
    }

private:
    NamespaceString _ns;
};
}  // namespace mongo
