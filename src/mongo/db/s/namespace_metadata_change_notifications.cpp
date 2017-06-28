/**
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/namespace_metadata_change_notifications.h"

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

    auto& notifList = _notificationsList[nss];
    notifToken->itToErase = notifList.insert(notifList.end(), notifToken);

    return {this, std::move(notifToken)};
}

void NamespaceMetadataChangeNotifications::notifyChange(const NamespaceString& nss) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    auto mapIt = _notificationsList.find(nss);
    if (mapIt == _notificationsList.end()) {
        return;
    }

    for (auto& notifToken : mapIt->second) {
        notifToken->notify.set();
        notifToken->itToErase.reset();
    }

    _notificationsList.erase(mapIt);
}

void NamespaceMetadataChangeNotifications::_unregisterNotificationToken(
    std::shared_ptr<NotificationToken> token) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    if (!token->itToErase) {
        return;
    }

    auto mapIt = _notificationsList.find(token->nss);
    auto& notifList = mapIt->second;
    notifList.erase(*token->itToErase);

    if (notifList.empty()) {
        _notificationsList.erase(mapIt);
    }
}

}  // namespace mongo
