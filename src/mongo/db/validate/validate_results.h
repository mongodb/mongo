/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace CollectionValidation {
// forwards declaring RepairMode to avoid adding validate_option to core headers
enum class RepairMode;
}  // namespace CollectionValidation

class ValidateResultsIf {
public:
    // Whether validation has been passed
    virtual bool isValid() const = 0;
    const StringSet& getErrors() const {
        return _errors;
    }
    StringSet* getErrorsUnsafe() {
        return &_errors;
    }
    const StringSet& getWarnings() const {
        return _warnings;
    }
    StringSet* getWarningsUnsafe() {
        return &_warnings;
    }

    // Returns true if and only if this was the first time |error| has been added to the set of
    // errors.
    bool addError(std::string error, bool stopValidation = true) {
        if (stopValidation) {
            _continueValidation = false;
        }
        return _errors.insert(std::move(error)).second;
    }

    // Returns true if and only if this was the first time |warning| has been added to the set of
    // warnings.
    bool addWarning(std::string warning) {
        return _warnings.insert(std::move(warning)).second;
    }

    // Returns true when the validation library should continue
    // running validation. May return true when isValid() returns false
    bool continueValidation() const {
        return _continueValidation;
    }

protected:
    bool _continueValidation = true;
    bool _fatalError = false;

    // Errors and warnings are sets to avoid creating duplicate findings in validation output.
    StringSet _errors;
    StringSet _warnings;
};

// Per-index validate results.
class IndexValidateResults final : public ValidateResultsIf {
public:
    bool isValid() const override {
        return _errors.empty();
    }

    int64_t getKeysTraversed() const {
        return _keysTraversed;
    }

    void addKeysTraversed(int64_t keysTraversed) {
        _keysTraversed += keysTraversed;
    }

    int64_t getKeysRemovedFromRecordStore() {
        return _keysRemovedFromRecordStore;
    }
    void addKeysRemovedFromRecordStore(int64_t keysRemovedFromRecordStore) {
        _keysRemovedFromRecordStore += keysRemovedFromRecordStore;
    }

    bool hasStructuralDamage() {
        return _hasStructuralDamage;
    }

    void setHasStructuralDamage(bool hasStructuralDamage) {
        _hasStructuralDamage = hasStructuralDamage;
    }

    BSONObj getSpec() const {
        return _spec;
    }

    void setSpec(BSONObj spec) {
        _spec = std::move(spec);
    }

private:
    int64_t _keysTraversed = 0;
    int64_t _keysRemovedFromRecordStore = 0;
    BSONObj _spec = {};
    bool _hasStructuralDamage = false;
};

using ValidateResultsMap = std::map<std::string, IndexValidateResults>;

// Validation results for an entire collection.
class ValidateResults final : public ValidateResultsIf {
public:
    bool isValid() const override {
        if (_errors.size())
            return false;
        for (const auto& [k, v] : getIndexResultsMap()) {
            if (!v.isValid()) {
                return false;
            }
        }
        return true;
    }

    void setNamespaceString(NamespaceString nss) {
        _nss = std::move(nss);
    }

    void setUUID(UUID uuid) {
        _uuid = std::move(uuid);
    }

    const std::vector<BSONObj>& getExtraIndexEntries() const {
        return _extraIndexEntries;
    }
    void addExtraIndexEntry(BSONObj entry);

    const std::vector<BSONObj>& getMissingIndexEntries() const {
        return _missingIndexEntries;
    }
    void addMissingIndexEntry(BSONObj entry);

    const std::vector<RecordId>& getCorruptRecords() const {
        return _corruptRecords;
    }
    void addCorruptRecord(RecordId record) {
        _corruptRecords.push_back(std::move(record));
    }

    bool getRepaired() const {
        return _repaired;
    }
    void setRepaired(bool repaired) {
        _repaired = repaired;
    }

    bool hasStructuralDamage() const {
        return _hasStructuralDamage;
    }

    void setHasStructuralDamage(bool hasStructuralDamage) {
        _hasStructuralDamage = hasStructuralDamage;
    }

    boost::optional<Timestamp> getReadTimestamp() const {
        return _readTimestamp;
    }
    void setReadTimestamp(boost::optional<Timestamp> readTimestampOpt) {
        _readTimestamp = std::move(readTimestampOpt);
    }

    void setNumInvalidDocuments(long long numInvalidDocuments) {
        _numInvalidDocuments = numInvalidDocuments;
    }

    void setNumNonCompliantDocuments(long long numNonCompliantDocuments) {
        _numNonCompliantDocuments = numNonCompliantDocuments;
    }

    void setNumRecords(long long numRecords) {
        _numRecords = numRecords;
    }

    const std::set<Timestamp>& getRecordTimestamps() const {
        return _recordTimestamps;
    }
    std::set<Timestamp>* getRecordTimestampsPtr() {
        return &_recordTimestamps;
    }
    void addRecordTimestamp(Timestamp recordTimestamp) {
        _recordTimestamps.insert(std::move(recordTimestamp));
    }

    long long getNumRemovedCorruptRecords() const {
        return _numRemovedCorruptRecords;
    }
    void addNumRemovedCorruptRecords(long long numRemovedCorruptRecords) {
        _numRemovedCorruptRecords += numRemovedCorruptRecords;
    }

    long long getNumRemovedExtraIndexEntries() const {
        return _numRemovedExtraIndexEntries;
    }
    void addNumRemovedExtraIndexEntries(long long numRemovedExtraIndexEntries) {
        _numRemovedExtraIndexEntries += numRemovedExtraIndexEntries;
    }

    long long getNumInsertedMissingIndexEntries() const {
        return _numInsertedMissingIndexEntries;
    }
    void addNumInsertedMissingIndexEntries(long long numInsertedMissingIndexEntries) {
        _numInsertedMissingIndexEntries += numInsertedMissingIndexEntries;
    }

    long long getNumDocumentsMovedToLostAndFound() const {
        return _numDocumentsMovedToLostAndFound;
    }
    void addNumDocumentsMovedToLostAndFound(long long numDocumentsMovedToLostAndFound) {
        _numDocumentsMovedToLostAndFound += numDocumentsMovedToLostAndFound;
    }

    long long getNumOutdatedMissingIndexEntry() const {
        return _numOutdatedMissingIndexEntry;
    }
    void addNumOutdatedMissingIndexEntry(long long numOutdatedMissingIndexEntry) {
        _numOutdatedMissingIndexEntry += numOutdatedMissingIndexEntry;
    }

    const ValidateResultsMap& getIndexResultsMap() const {
        return _indexResultsMap;
    }

    ValidateResultsMap& getIndexResultsMap() {
        return _indexResultsMap;
    }

    IndexValidateResults& getIndexValidateResult(const std::string& indexName) {
        return _indexResultsMap[indexName];
    }

    std::string getRepairMode() const {
        return _repairMode;
    }

    void setRepairMode(CollectionValidation::RepairMode mode);

    // Takes a bool that indicates the context of the caller and a BSONObjBuilder to append with
    // validate results.
    void appendToResultObj(
        BSONObjBuilder* resultObj,
        bool debugging,
        const SerializationContext& sc = SerializationContext::stateCommandReply()) const;

private:
    boost::optional<UUID> _uuid;
    boost::optional<NamespaceString> _nss;

    size_t _extraIndexEntriesUsedBytes = 0;
    size_t _missingIndexEntriesUsedBytes = 0;
    std::vector<BSONObj> _extraIndexEntries;
    std::vector<BSONObj> _missingIndexEntries;
    std::vector<RecordId> _corruptRecords;

    bool _repaired = false;
    std::string _repairMode;
    bool _hasStructuralDamage = false;
    boost::optional<Timestamp> _readTimestamp = boost::none;

    // Collection stats.
    // If validate doesn't progress far enough to determine these, they will remain nullopt.
    boost::optional<long long> _numInvalidDocuments;
    boost::optional<long long> _numNonCompliantDocuments;
    boost::optional<long long> _numRecords;

    // Timestamps (startTs, startDurable, stopTs, stopDurableTs) related to records
    // with validation errors. See WiredTigerRecordStore::printRecordMetadata().
    std::set<Timestamp> _recordTimestamps;
    long long _numRemovedCorruptRecords = 0;
    long long _numRemovedExtraIndexEntries = 0;
    long long _numInsertedMissingIndexEntries = 0;
    long long _numDocumentsMovedToLostAndFound = 0;
    long long _numOutdatedMissingIndexEntry = 0;

    // Maps index names to index-specific validation results.
    ValidateResultsMap _indexResultsMap;
};

}  // namespace mongo
