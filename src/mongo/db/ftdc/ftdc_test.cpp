/**
 * Copyright (C) 2015 MongoDB Inc.
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
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/ftdc_test.h"

#include <boost/filesystem.hpp>

#include "mongo/base/data_type_validated.h"
#include "mongo/base/init.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/db/client.h"
#include "mongo/db/ftdc/file_reader.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

void ValidateDocumentList(const boost::filesystem::path& p, const std::vector<BSONObj>& docs) {
    FTDCFileReader reader;

    ASSERT_OK(reader.open(p));

    std::vector<BSONObj> list;
    auto sw = reader.hasNext();
    while (sw.isOK() && sw.getValue()) {
        list.emplace_back(std::get<1>(reader.next()).getOwned());
        sw = reader.hasNext();
    }

    ValidateDocumentList(list, docs);
}

void ValidateDocumentList(const std::vector<BSONObj>& docs1, const std::vector<BSONObj>& docs2) {
    ASSERT_EQUALS(docs1.size(), docs2.size());

    auto ai = docs1.begin();
    auto bi = docs2.begin();

    while (ai != docs1.end() && bi != docs2.end()) {
        if (!(*ai == *bi)) {
            std::cout << *ai << " vs " << *bi << std::endl;
            ASSERT_TRUE(*ai == *bi);
        }
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

MONGO_INITIALIZER_WITH_PREREQUISITES(FTDCTestInit, ("ThreadNameInitializer"))
(InitializerContext* context) {
    setGlobalServiceContext(stdx::make_unique<ServiceContextNoop>());

    getGlobalServiceContext()->setFastClockSource(stdx::make_unique<ClockSourceMock>());
    getGlobalServiceContext()->setPreciseClockSource(stdx::make_unique<ClockSourceMock>());
    getGlobalServiceContext()->setTickSource(stdx::make_unique<TickSourceMock>());

    Client::initThreadIfNotAlready("UnitTest");

    return Status::OK();
}

}  // namespace mongo
