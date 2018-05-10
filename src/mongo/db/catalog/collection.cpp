/*-
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"

#include "mongo/base/counter.h"
#include "mongo/base/owned_pointer_map.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/update/update_driver.h"

#include "mongo/db/auth/user_document_parser.h"  // XXX-ANDY
#include "mongo/rpc/object_check.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

namespace mongo {
// Emit the vtable in this TU
Collection::Impl::~Impl() = default;

MONGO_DEFINE_SHIM(Collection::makeImpl);

MONGO_DEFINE_SHIM(Collection::parseValidationLevel);

MONGO_DEFINE_SHIM(Collection::parseValidationAction);

void Collection::TUHook::hook() noexcept {}

std::string CompactOptions::toString() const {
    std::stringstream ss;
    ss << "paddingMode: ";
    switch (paddingMode) {
        case NONE:
            ss << "NONE";
            break;
        case PRESERVE:
            ss << "PRESERVE";
            break;
        case MANUAL:
            ss << "MANUAL (" << paddingBytes << " + ( doc * " << paddingFactor << ") )";
    }

    ss << " validateDocuments: " << validateDocuments;

    return ss.str();
}

//
// CappedInsertNotifier
//

CappedInsertNotifier::CappedInsertNotifier() : _version(0), _dead(false) {}

void CappedInsertNotifier::notifyAll() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    ++_version;
    _notifier.notify_all();
}

void CappedInsertNotifier::waitUntil(uint64_t prevVersion, Date_t deadline) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (!_dead && prevVersion == _version) {
        if (stdx::cv_status::timeout == _notifier.wait_until(lk, deadline.toSystemTimePoint())) {
            return;
        }
    }
}

void CappedInsertNotifier::kill() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _dead = true;
    _notifier.notify_all();
}

bool CappedInsertNotifier::isDead() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _dead;
}

// ----

}  // namespace mongo
