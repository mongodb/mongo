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

#pragma once

#include <list>
#include <map>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/notification.h"

namespace mongo {

class OperationContext;

/**
 * Map of one-snot notifications for changes to a particular namespace.
 */
class NamespaceMetadataChangeNotifications {
    MONGO_DISALLOW_COPYING(NamespaceMetadataChangeNotifications);

    struct NotificationToken;

    friend class ScopedNotification;

public:
    NamespaceMetadataChangeNotifications();
    ~NamespaceMetadataChangeNotifications();

    /**
     * Holds the token for a registered metadata change notification and unregisters it when it gets
     * out of scope, if it has not been signalled yet.
     */
    class ScopedNotification {
        MONGO_DISALLOW_COPYING(ScopedNotification);

    public:
        ScopedNotification(NamespaceMetadataChangeNotifications* notifications,
                           std::shared_ptr<NotificationToken> token)
            : _notifications(notifications), _token(std::move(token)) {}

        ScopedNotification(ScopedNotification&&) = default;

        ~ScopedNotification() {
            if (_token) {
                _notifications->_unregisterNotificationToken(std::move(_token));
            }
        }

        void get(OperationContext* opCtx) {
            _token->notify.get(opCtx);
        }

    private:
        NamespaceMetadataChangeNotifications* _notifications;

        std::shared_ptr<NamespaceMetadataChangeNotifications::NotificationToken> _token;
    };

    /**
     * Creates and registers a new pending notification for the specified namespace.
     */
    ScopedNotification createNotification(const NamespaceString& nss);

    /**
     * Goes through all registered notifications for this namespace signals them and removes them
     * from the registry atomically.
     */
    void notifyChange(const NamespaceString& nss);

private:
    using NotificationsList = std::list<std::shared_ptr<NotificationToken>>;

    struct NotificationToken {
        NotificationToken(NamespaceString inNss) : nss(std::move(inNss)) {}

        NamespaceString nss;

        Notification<void> notify;

        boost::optional<NamespaceMetadataChangeNotifications::NotificationsList::iterator>
            itToErase;
    };

    void _unregisterNotificationToken(std::shared_ptr<NotificationToken> token);

    stdx::mutex _mutex;
    std::map<NamespaceString, NotificationsList> _notificationsList;
};

}  // namespace mongo
