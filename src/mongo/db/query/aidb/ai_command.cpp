/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/aidb/ai_command.h"

#include <algorithm>
#include <stdexcept>

#include "mongo/bson/json.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/query/aidb/ai_data_generator.h"
#include "mongo/db/query/aidb/ai_database.h"
#include "mongo/db/query/aidb/ai_index_accessor.h"

namespace mongo::ai {

stdx::unordered_map<std::string, std::unique_ptr<Command>> Command::_registeredCommands{};

namespace {
Command::Register<GenerateCommand> ___0{};
Command::Register<DemoCommand> ___1{};
Command::Register<UseCommand> ___2{};
Command::Register<ShowCommand> ___3{};
Command::Register<GetCommand> ___4{};
Command::Register<CollectionInfoCommand> ___5{};
Command::Register<IndexScanCommand> ___6{};
Command::Register<CollectionScanCommand> ___7{};
Command::Register<CreateCollectionCommand> ___8{};
Command::Register<CreateIndexCommand> ___9{};

void printFoundRecords(std::ostream& os, const std::vector<BSONObj>& records) {
    constexpr size_t MaxRecordsToShow = 20;

    size_t recordsToShow = std::min(MaxRecordsToShow, records.size());
    std::cout << "Found " << records.size() << " records" << std::endl;
    std::cout << "Showed " << recordsToShow << " records" << std::endl;
    for (size_t i = 0; i < recordsToShow; ++i) {
        std::cout << records[i] << std::endl;
    }
}

bool contains(const BSONObj& key, const BSONObj& doc) {
    for (const BSONElement& keyElement : key) {
        StringData fieldName = keyElement.fieldNameStringData();
        if (!doc.hasField(fieldName) ||
            (BSONElement::compareElements(doc[fieldName], keyElement, 0, nullptr) != 0)) {
            return false;
        }
    }
    return true;
}
}  // namespace

Command* Command::getCommand(const std::string& name) {
    auto pos = _registeredCommands.find(name);
    if (pos != _registeredCommands.end()) {
        return pos->second.get();
    }

    return nullptr;
}

Status Command::execute(OperationContext* opCtx, const std::string& commandString) {
    std::string name;
    std::istringstream iss{commandString};
    if (getline(iss, name, ' ')) {
        auto* command = getCommand(name);
        if (command != nullptr) {
            try {
                return command->execute(opCtx, iss);
            } catch (const DBException& dbex) {
                return dbex.toStatus();
            } catch (const std::exception& ex) {
                std::ostringstream oss;
                oss << command->usage() << "  ---  Error: " << ex.what();
                return Status{ErrorCodes::BadValue, oss.str()};
            }
        }
    }

    return Status{ErrorCodes::BadValue, "Command not found"};
}

void Command::help(std::ostream& os) {
    for (const auto& p : _registeredCommands) {
        os << '\t' << p.second->help() << std::endl;
    }
}

std::vector<std::string> Command::split(std::istream& is, size_t numParts) {
    std::vector<std::string> result{};
    result.reserve(numParts);

    std::string token{};
    while (numParts > 1 && getline(is, token, ' ')) {
        --numParts;
        result.push_back(token);
    }

    if (getline(is, token)) {
        result.push_back(token);
    }

    return result;
}

Status GenerateCommand::execute(OperationContext* opCtx, std::istream& commandStream) {
    auto parts = split(commandStream, 2);
    if (parts.size() != 2) {
        return Status{ErrorCodes::Error::BadValue, usage()};
    }

    Database db{opCtx};

    NamespaceString nss{db.getNamespaceString(parts[0])};
    size_t numberOfDocuments = std::stoull(parts[1]);

    db.ensureCollection(nss);

    DataGenerator gen{};
    auto docs = gen.generateDocuments(5, 25, numberOfDocuments);
    db.insertDocuments(nss, docs);

    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableCheckpoint*/ false);

    return Status::OK();
}

Status DemoCommand::execute(OperationContext* opCtx, std::istream& commandStream) {
    auto parts = split(commandStream, 2);
    if (parts.size() != 2) {
        return Status{ErrorCodes::Error::BadValue, usage()};
    }

    std::string collectionName = parts[0];
    size_t numberOfDocuments = std::stoull(parts[1]);

    Database db{opCtx};

    NamespaceString nss{db.getNamespaceString(collectionName)};
    StringData indexOnDataName{"indexOnData"};
    db.createCollection(nss);
    db.createIndex(nss, BSON("data" << 1), indexOnDataName);

    DataGenerator gen{};
    auto docs = gen.generateDocuments(5, 25, numberOfDocuments);
    db.insertDocuments(nss, docs);

    opCtx->recoveryUnit()->waitUntilUnjournaledWritesDurable(opCtx, /*stableCheckpoint*/ false);

    IndexAccessor index{opCtx, nss, indexOnDataName};

    std::string dataKeyValue = docs[docs.size() * 5 / 7]["data"].str();
    auto result = index.findAll(BSON("data" << dataKeyValue.c_str()));

    for (const auto& doc : result) {
        std::cout << doc << std::endl;
    }

    return Status::OK();
}

Status UseCommand::execute(OperationContext* opCtx, std::istream& commandStream) {
    auto parts = split(commandStream, 1);
    if (parts.size() != 1) {
        return Status{ErrorCodes::Error::BadValue, usage()};
    }

    Database::setCurrentDbName(opCtx, parts[0]);

    return Status::OK();
}

Status ShowCommand::execute(OperationContext* opCtx, std::istream& commandStream) {
    auto parts = split(commandStream, 1);
    if (parts.size() != 1) {
        return Status{ErrorCodes::Error::BadValue, usage()};
    }

    std::shared_ptr<const CollectionCatalog> catalog = CollectionCatalog::get(opCtx);
    if (parts[0] == "collections") {
        auto dbName = Database::getCurrentDbName(opCtx);
        Lock::DBLock dbLock(opCtx, dbName, MODE_S);
        for (const auto& colname : catalog->getAllCollectionNamesFromDb(opCtx, dbName)) {
            std::cout << colname << std::endl;
        }
    } else if (parts[0] == "databases") {
        for (const auto& dbname : catalog->getAllDbNames()) {
            std::cout << dbname << std::endl;
        }
    } else {
        return Status{ErrorCodes::Error::BadValue, usage()};
    }

    return Status::OK();
}

Status GetCommand::execute(OperationContext* opCtx, std::istream& commandStream) {
    auto parts = split(commandStream, 2);
    if (parts.size() != 2) {
        return Status{ErrorCodes::Error::BadValue, usage()};
    }

    std::string collectionName = parts[0];
    RecordId recordId{std::stoll(parts[1])};

    Database db{opCtx};

    AutoGetCollectionForRead autoColl{opCtx, db.getNamespaceString(collectionName)};
    uassert(7777701, "collection is not found", autoColl);
    RecordStore* recordStore = autoColl->getRecordStore();

    std::unique_ptr<SeekableRecordCursor> cursor = recordStore->getCursor(opCtx);
    boost::optional<Record> record = cursor->seekExact(recordId);
    uassert(7777702, "record id is not found", record);

    std::cout << record->data.toBson() << std::endl;

    return Status::OK();
}

Status CollectionInfoCommand::execute(OperationContext* opCtx, std::istream& commandStream) {
    auto parts = split(commandStream, 1);
    if (parts.size() != 1) {
        return Status{ErrorCodes::Error::BadValue, usage()};
    }

    Database db{opCtx};

    AutoGetCollectionForRead autoColl{opCtx, db.getNamespaceString(parts[0])};
    uassert(7777703, "collection is not found", autoColl);
    const CollectionPtr& coll = autoColl.getCollection();

    RecordStore* recordStore = coll->getRecordStore();
    std::cout << "Name: " << recordStore->ns() << std::endl;
    std::cout << "Number of Records: " << recordStore->numRecords(opCtx) << std::endl;
    std::cout << "Data size: " << recordStore->dataSize(opCtx) << std::endl;
    std::cout << "Is capped: " << coll->isCapped() << std::endl;
    std::cout << "Storage size: " << recordStore->storageSize(opCtx) << std::endl;
    std::cout << "Free storage size: " << recordStore->freeStorageSize(opCtx) << std::endl;

    std::cout << std::endl << "Indices" << std::endl;
    const IndexCatalog* indexCatalog = coll->getIndexCatalog();
    std::unique_ptr<IndexCatalog::IndexIterator> indexIter =
        indexCatalog->getIndexIterator(opCtx, /*includeUnfinishedIndexes*/ true);
    while (indexIter->more()) {
        const IndexCatalogEntry* indexEntry = indexIter->next();
        const IndexDescriptor* indexDesc = indexEntry->descriptor();

        std::cout << indexDesc->indexName() << " " << indexDesc->keyPattern()
                  << " ready: " << indexEntry->isReady(opCtx, coll)
                  << " isMultiKey: " << indexEntry->isMultikey(opCtx, coll) << std::endl;
    }

    return Status::OK();
}

Status IndexScanCommand::execute(OperationContext* opCtx, std::istream& commandStream) {
    auto parts = split(commandStream, 3);
    if (parts.size() != 3) {
        return Status{ErrorCodes::Error::BadValue, usage()};
    }

    Database db{opCtx};

    NamespaceString nss = db.getNamespaceString(parts[0]);
    StringData indexName{parts[1]};
    BSONObj key = fromjson(parts[2]);

    IndexAccessor indexAccessor{opCtx, nss, indexName};
    auto found = indexAccessor.findAll(key);

    printFoundRecords(std::cout, found);

    return Status::OK();
}

Status CollectionScanCommand::execute(OperationContext* opCtx, std::istream& commandStream) {
    auto parts = split(commandStream, 2);
    if (parts.size() != 2) {
        return Status{ErrorCodes::Error::BadValue, usage()};
    }

    Database db{opCtx};

    NamespaceString nss = db.getNamespaceString(parts[0]);
    BSONObj key = fromjson(parts[1]);

    AutoGetCollectionForRead autoColl{opCtx, nss};
    uassert(7777704, "collection is not found", autoColl);
    const CollectionPtr& coll = autoColl.getCollection();

    std::vector<BSONObj> records{};
    RecordStore* recordStore = coll->getRecordStore();
    std::unique_ptr<SeekableRecordCursor> cursor = recordStore->getCursor(opCtx);
    boost::optional<Record> record{};
    while (record = cursor->next(), record) {
        BSONObj doc = record->data.toBson();
        if (contains(key, doc)) {
            records.push_back(doc);
        }
    }

    printFoundRecords(std::cout, records);
    return Status::OK();
}

Status CreateCollectionCommand::execute(OperationContext* opCtx, std::istream& commandStream) {
    auto parts = split(commandStream, 1);
    if (parts.size() != 1) {
        return Status{ErrorCodes::Error::BadValue, usage()};
    }

    Database db{opCtx};

    NamespaceString nss = db.getNamespaceString(parts[0]);
    db.createCollection(nss);

    return Status::OK();
}

Status CreateIndexCommand::execute(OperationContext* opCtx, std::istream& commandStream) {
    auto parts = split(commandStream, 3);
    if (parts.size() != 3) {
        return Status{ErrorCodes::Error::BadValue, usage()};
    }

    Database db{opCtx};

    NamespaceString nss = db.getNamespaceString(parts[0]);
    StringData indexName{parts[1]};
    BSONObj pattern = fromjson(parts[2]);

    db.ensureCollection(nss);
    db.createIndex(nss, pattern, indexName);

    return Status::OK();
}
}  // namespace mongo::ai
