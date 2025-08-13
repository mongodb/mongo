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

#include "mongo/db/global_catalog/catalog_cache/namespace_metadata_change_notifications.h"

#include "mongo/util/assert_util.h"

#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

NamespaceMetadataChangeNotifications::NamespaceMetadataChangeNotifications() = default;

NamespaceMetadataChangeNotifications::~NamespaceMetadataChangeNotifications() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_notificationsList.empty());
}

NamespaceMetadataChangeNotifications::ScopedNotification
NamespaceMetadataChangeNotifications::createNotification(const NamespaceString& nss) {
    auto notifToken = std::make_shared<NotificationToken>(nss);

    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto& notifList = _notificationsList[nss].second;
    notifToken->itToErase = notifList.insert(notifList.end(), notifToken);

    return {this, std::move(notifToken)};
}

Timestamp NamespaceMetadataChangeNotifications::get(OperationContext* opCtx,
                                                    ScopedNotification& notif) {
    // Wait for notification to be ready
    notif.get(opCtx);

    // Get value and replace notification token under lock
    auto nss = notif.getToken()->nss;
    auto newToken = std::make_shared<NotificationToken>(nss);

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto& [opTime, notifList] = _notificationsList[nss];

    // Put new token in _notificationsList
    newToken->itToErase = notifList.insert(notifList.end(), newToken);

    // Deregister old token from notifications list.
    _unregisterNotificationToken_inlock(lock, *notif.getToken());

    // Update scoped notification.
    notif.replaceToken(std::move(newToken));

    return opTime;
}

void NamespaceMetadataChangeNotifications::notifyChange(const NamespaceString& nss,
                                                        const Timestamp& commitTime) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    auto mapIt = _notificationsList.find(nss);
    if (mapIt == _notificationsList.end()) {
        return;
    }

    auto& [opTime, notifList] = mapIt->second;

    if (commitTime <= opTime)
        return;

    opTime = commitTime;
    for (auto& notifToken : notifList) {
        if (!notifToken->notify)
            notifToken->notify.set();
    }
}

void NamespaceMetadataChangeNotifications::_unregisterNotificationToken(
    const NotificationToken& token) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    _unregisterNotificationToken_inlock(lg, token);
}

void NamespaceMetadataChangeNotifications::_unregisterNotificationToken_inlock(
    WithLock lk, const NotificationToken& token) {
    auto mapIt = _notificationsList.find(token.nss);
    if (mapIt == _notificationsList.end()) {
        return;
    }
    auto& notifList = mapIt->second.second;
    notifList.erase(*token.itToErase);

    if (notifList.empty()) {
        _notificationsList.erase(mapIt);
    }
}

}  // namespace mongo
