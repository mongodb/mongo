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

#include "mongo/db/storage/biggie/biggie_sorted_impl.h"

#include <set>

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace biggie {

Status SortedDataBuilderInterface::addKey(const BSONObj& key, const RecordId& loc) {
    return Status::OK();
}

SortedDataBuilderInterface* SortedDataInterface::getBulkBuilder(OperationContext* opCtx,
                                                                bool dupsAllowed) {
    return new SortedDataBuilderInterface();  // TODO: return real thing.
}

Status SortedDataInterface::insert(OperationContext* opCtx,
                                   const BSONObj& key,
                                   const RecordId& loc,
                                   bool dupsAllowed) {
    return Status::OK();  // TODO: Implement.
}

void SortedDataInterface::unindex(OperationContext* opCtx,
                                  const BSONObj& key,
                                  const RecordId& loc,
                                  bool dupsAllowed) {
    return;  // TODO: Implement.
}

Status SortedDataInterface::dupKeyCheck(OperationContext* opCtx,
                                        const BSONObj& key,
                                        const RecordId& loc) {
    return Status::OK();
    // TODO: Implement.
}

void SortedDataInterface::fullValidate(OperationContext* opCtx,
                                       long long* numKeysOut,
                                       ValidateResults* fullResults) const {
    // TODO: Implement.
}

bool SortedDataInterface::appendCustomStats(OperationContext* opCtx,
                                            BSONObjBuilder* output,
                                            double scale) const {
    return false;  // TODO: Implement.
}

long long SortedDataInterface::getSpaceUsedBytes(OperationContext* opCtx) const {
    return -1;  // TODO: Implement.
}

bool SortedDataInterface::isEmpty(OperationContext* opCtx) {
    return true;  // TODO: Implement.
}

std::unique_ptr<mongo::SortedDataInterface::Cursor> SortedDataInterface::newCursor(
    OperationContext* opCtx, bool isForward) const {
    return std::make_unique<SortedDataInterface::Cursor>(opCtx, isForward);  // TODO: Implement.
}

Status SortedDataInterface::initAsEmpty(OperationContext* opCtx) {
    return Status::OK();  // TODO: Implement.
}

// Cursor
SortedDataInterface::Cursor::Cursor(OperationContext* opCtx, bool isForward)
    : _opCtx(opCtx), _isForward(isForward) {}

void SortedDataInterface::Cursor::setEndPosition(const BSONObj& key, bool inclusive) {
    return;
}

boost::optional<IndexKeyEntry> SortedDataInterface::Cursor::next(RequestedInfo parts) {
    return boost::none;
}

boost::optional<IndexKeyEntry> SortedDataInterface::Cursor::seek(const BSONObj& key,
                                                                 bool inclusive,
                                                                 RequestedInfo parts) {
    return boost::none;
}

boost::optional<IndexKeyEntry> SortedDataInterface::Cursor::seek(const IndexSeekPoint& seekPoint,
                                                                 RequestedInfo parts) {
    return boost::none;
}

void SortedDataInterface::Cursor::save() {
    return;
}

void SortedDataInterface::Cursor::restore() {
    return;
}

void SortedDataInterface::Cursor::detachFromOperationContext() {
    return;
}

void SortedDataInterface::Cursor::reattachToOperationContext(OperationContext* opCtx) {
    return;
}

}  // namespace biggie
}  // namespace mongo
