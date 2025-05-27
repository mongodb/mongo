/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/ftdc/ftdc_test.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/ftdc/file_reader.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <tuple>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>

namespace mongo {

namespace {

BSONObj filteredFTDCCopy(const BSONObj& obj) {
    BSONObjBuilder builder;
    for (const auto& f : obj) {
        if (FTDCBSONUtil::isFTDCType(f.type())) {
            builder.append(f);
        }
    }
    return builder.obj();
}

using ReaderResult = decltype(std::declval<FTDCFileReader>().next());

template <typename T, typename Func>
void readFile(FTDCFileReader&& reader,
              const boost::filesystem::path& path,
              const Func& mapper,
              std::vector<T>& docs) {
    auto readerHasNext = [&] {
        auto sw = reader.hasNext();
        ASSERT_OK(sw) << "corrupt FTDC file: " << path;
        return sw.getValue();
    };
    while (readerHasNext()) {
        docs.push_back(mapper(reader.next()));
    }
}

template <typename T, typename Func>
std::vector<T> loadFTDCAsVector(const boost::filesystem::path& path, const Func& mapper) {
    FTDCFileReader reader;
    ASSERT_OK(reader.open(path));

    std::vector<T> docs;
    readFile(std::move(reader), path, mapper, docs);

    return docs;
}

template <typename Func, typename T = std::invoke_result_t<Func, ReaderResult>>
std::vector<T> loadFTDCAsVector(const std::vector<boost::filesystem::path>& paths,
                                const Func& mapper) {
    std::vector<T> docs;
    for (const auto& path : paths) {
        FTDCFileReader reader;
        ASSERT_OK(reader.open(path));

        readFile(std::move(reader), path, mapper, docs);
    }
    return docs;
}

std::vector<BSONObj> loadFTDCAsDocumentVector(const boost::filesystem::path& path) {
    return loadFTDCAsVector<BSONObj>(path, [](const ReaderResult& result) {
        auto&& [type, doc, date] = result;
        return doc.getOwned();
    });
}

using TypeAndDocument = std::pair<FTDCBSONUtil::FTDCType, BSONObj>;

std::vector<TypeAndDocument> loadFTDCAsTypeAndDocumentVector(
    const std::vector<boost::filesystem::path>& paths) {
    return loadFTDCAsVector(paths, [](const ReaderResult& result) {
        auto&& [type, doc, date] = result;
        return TypeAndDocument{type, doc.getOwned()};
    });
}

void compareFTDCDocuments(FTDCValidationMode mode, BSONObj actual, BSONObj expected) {
    if (mode == FTDCValidationMode::kStrict) {
        if (SimpleBSONObjComparator::kInstance.evaluate(actual != expected)) {
            std::cout << actual << " vs " << expected << std::endl;
            ASSERT_BSONOBJ_EQ(actual, expected);
        }
    } else {
        BSONObj left = filteredFTDCCopy(actual);
        BSONObj right = filteredFTDCCopy(expected);
        if (SimpleBSONObjComparator::kInstance.evaluate(left != right)) {
            std::cout << left << " vs " << right << std::endl;
            ASSERT_BSONOBJ_EQ(left, right);
        }
    }
}

void ValidateDocumentListByType(const std::vector<std::pair<FTDCBSONUtil::FTDCType, BSONObj>>& docs,
                                const std::vector<BSONObj>& expectedOnRotateMetadata,
                                const std::vector<BSONObj>& expectedMetrics,
                                const std::vector<BSONObj>& expectedPeriodicMetadata,
                                FTDCValidationMode mode) {
    auto expectedSize =
        expectedOnRotateMetadata.size() + expectedMetrics.size() + expectedPeriodicMetadata.size();
    ASSERT_EQUALS(docs.size(), expectedSize);

    auto ai = docs.begin();
    auto bi = expectedOnRotateMetadata.begin();
    auto ci = expectedMetrics.begin();
    auto di = expectedPeriodicMetadata.begin();
    while (ai != docs.end()) {
        switch (ai->first) {
            case FTDCBSONUtil::FTDCType::kMetadata:
                ASSERT_FALSE(bi == expectedOnRotateMetadata.end());
                compareFTDCDocuments(mode, ai->second, *bi);
                bi++;
                break;
            case FTDCBSONUtil::FTDCType::kMetricChunk:
                ASSERT_FALSE(ci == expectedMetrics.end());
                compareFTDCDocuments(mode, ai->second, *ci);
                ci++;
                break;
            case FTDCBSONUtil::FTDCType::kPeriodicMetadata:
                ASSERT_FALSE(di == expectedPeriodicMetadata.end());
                compareFTDCDocuments(mode, ai->second, *di);
                di++;
                break;
            default:
                MONGO_UNREACHABLE;
        }
        ai++;
    }

    ASSERT_TRUE(ai == docs.end());
    ASSERT_TRUE(bi == expectedOnRotateMetadata.end());
    ASSERT_TRUE(ci == expectedMetrics.end());
    ASSERT_TRUE(di == expectedPeriodicMetadata.end());
}

}  // namespace

void ValidateDocumentListByType(const std::vector<boost::filesystem::path>& paths,
                                const std::vector<BSONObj>& expectedOnRotateMetadata,
                                const std::vector<BSONObj>& expectedMetrics,
                                const std::vector<BSONObj>& expectedPeriodicMetadata,
                                FTDCValidationMode mode) {
    auto list = loadFTDCAsTypeAndDocumentVector(paths);
    ValidateDocumentListByType(
        list, expectedOnRotateMetadata, expectedMetrics, expectedPeriodicMetadata, mode);
}

void ValidateDocumentList(const boost::filesystem::path& path,
                          const std::vector<BSONObj>& docs,
                          FTDCValidationMode mode) {
    auto list = loadFTDCAsDocumentVector(path);
    ValidateDocumentList(list, docs, mode);
}

void ValidateDocumentList(const std::vector<BSONObj>& docs1,
                          const std::vector<BSONObj>& docs2,
                          FTDCValidationMode mode) {
    ASSERT_EQUALS(docs1.size(), docs2.size());

    auto ai = docs1.begin();
    auto bi = docs2.begin();

    while (ai != docs1.end() && bi != docs2.end()) {
        compareFTDCDocuments(mode, *ai, *bi);
        ++ai;
        ++bi;
    }

    ASSERT_TRUE(ai == docs1.end() && bi == docs2.end());
}


void deleteFileIfNeeded(const boost::filesystem::path& p) {
    if (boost::filesystem::exists(p)) {
        boost::filesystem::remove(p);
    }
}

std::vector<boost::filesystem::path> scanDirectory(const boost::filesystem::path& path) {
    boost::filesystem::directory_iterator di(path);
    std::vector<boost::filesystem::path> files;

    for (; di != boost::filesystem::directory_iterator(); di++) {
        boost::filesystem::directory_entry& de = *di;
        auto f = de.path().filename();
        files.emplace_back(path / f);
    }

    std::sort(files.begin(), files.end());

    return files;
}

void createDirectoryClean(const boost::filesystem::path& dir) {
    boost::filesystem::remove_all(dir);

    boost::filesystem::create_directory(dir);
}

}  // namespace mongo
