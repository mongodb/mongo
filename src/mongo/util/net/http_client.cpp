/**
 * Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/util/net/http_client.h"

#include <string>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"

namespace mongo {

Future<DataBuilder> HttpClient::postAsync(executor::ThreadPoolTaskExecutor* executor,
                                          StringData url,
                                          std::shared_ptr<std::vector<std::uint8_t>> data) const {
    auto pf = makePromiseFuture<DataBuilder>();
    std::string urlString(url.toString());

    auto status = executor->scheduleWork([
        shared_promise = pf.promise.share(),
        urlString = std::move(urlString),
        data = std::move(data),
        this
    ](const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {
        ConstDataRange cdr(reinterpret_cast<char*>(data->data()), data->size());
        try {
            auto result = this->post(urlString, cdr);
            shared_promise.emplaceValue(std::move(result));
        } catch (...) {
            shared_promise.setError(exceptionToStatus());
        }
    });

    uassertStatusOK(status);
    return std::move(pf.future);
}

Future<DataBuilder> HttpClient::getAsync(executor::ThreadPoolTaskExecutor* executor,
                                         StringData url) const {
    auto pf = makePromiseFuture<DataBuilder>();
    std::string urlString(url.toString());

    auto status = executor->scheduleWork([ shared_promise = pf.promise.share(), urlString, this ](
        const executor::TaskExecutor::CallbackArgs& cbArgs) mutable {
        try {
            auto result = this->get(urlString);
            shared_promise.emplaceValue(std::move(result));
        } catch (...) {
            shared_promise.setError(exceptionToStatus());
        }
    });

    uassertStatusOK(status);
    return std::move(pf.future);
}

}  // namespace mongo
