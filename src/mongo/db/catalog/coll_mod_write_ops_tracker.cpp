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

#include "mongo/db/catalog/coll_mod_write_ops_tracker.h"

namespace mongo {

namespace {

auto getCollModWriteOpsTracker = ServiceContext::declareDecoration<CollModWriteOpsTracker>();

}  // namespace

// static
CollModWriteOpsTracker* CollModWriteOpsTracker::get(ServiceContext* service) {
    return &getCollModWriteOpsTracker(service);
}

void CollModWriteOpsTracker::onDocumentChanged(const UUID& uuid, const BSONObj& doc) {
    stdx::lock_guard<Latch> lock(_mutex);
    if (_tokens.empty()) {
        return;
    }

    for (const auto token : _tokens) {
        if (token->_uuid != uuid) {
            continue;
        }
        token->_docs->push_back(doc.getOwned());
    }
}

std::unique_ptr<CollModWriteOpsTracker::Token> CollModWriteOpsTracker::startTracking(
    const UUID& uuid) {
    stdx::lock_guard<Latch> lock(_mutex);
    auto listener = std::make_unique<Token>(this, uuid);
    _tokens.push_back(listener.get());
    return listener;
}

/**
 * Stops tracking write ops on a namespace.
 */
std::unique_ptr<CollModWriteOpsTracker::Docs> CollModWriteOpsTracker::stopTracking(
    std::unique_ptr<Token> token) {
    stdx::lock_guard<Latch> lock(_mutex);
    _tokens.remove(token.get());
    return std::move(token->_docs);
}

void CollModWriteOpsTracker::onTokenDestroyed(Token* token) {
    stdx::lock_guard<Latch> lock(_mutex);
    _tokens.remove(token);
}

CollModWriteOpsTracker::Token::Token(CollModWriteOpsTracker* tracker, const UUID& uuid)
    : _tracker(tracker), _uuid(uuid), _docs(std::make_unique<Docs>()) {}

CollModWriteOpsTracker::Token::~Token() {
    // '_docs' can only be invalidated by stopTracking().
    if (!_docs) {
        return;
    }
    _tracker->onTokenDestroyed(this);
}

}  // namespace mongo
