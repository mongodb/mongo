// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/routing_cache/namespace_metadata_change_notifications.h"

#include "mongo/util/assert_util.h"

#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

NamespaceMetadataChangeNotifications::NamespaceMetadataChangeNotifications() = default;

NamespaceMetadataChangeNotifications::~NamespaceMetadataChangeNotifications() {
    std::lock_guard<std::mutex> lock(_mutex);
    invariant(_notificationsList.empty());
}

NamespaceMetadataChangeNotifications::ScopedNotification
NamespaceMetadataChangeNotifications::createNotification(const NamespaceString& nss) {
    auto notifToken = std::make_shared<NotificationToken>(nss);

    std::lock_guard<std::mutex> lg(_mutex);

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

    std::lock_guard<std::mutex> lock(_mutex);
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
    std::lock_guard<std::mutex> lock(_mutex);

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
    std::lock_guard<std::mutex> lg(_mutex);

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
