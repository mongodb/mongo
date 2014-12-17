/*    Copyright 2014 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/util/net/ssl_expiration.h"

#include <string>

#include "mongo/util/log.h"

namespace mongo {

    static const unsigned long long dayInMillis = 24 * 60 * 60 * 1000;

    CertificateExpirationMonitor::CertificateExpirationMonitor(Date_t date)
        : _certExpiration(date)
        , _lastCheckTime(Date_t(curTimeMillis64())) {
    }

    std::string CertificateExpirationMonitor::taskName() const {
        return "CertificateExpirationMonitor";
    }

    void CertificateExpirationMonitor::taskDoWork() {
        const unsigned long long timeSinceLastCheck = 
            curTimeMillis64() - _lastCheckTime.millis;

        if (timeSinceLastCheck < dayInMillis)
            return;

        const Date_t now = Date_t(curTimeMillis64());
        _lastCheckTime = now;

        if (_certExpiration.millis <= now.millis) {
            // The certificate has expired.
            warning() << "Server certificate is now invalid. It expired on "
                      << dateToCtimeString(_certExpiration);
            return;
        }

        const unsigned long long remainingValidMillis =
            _certExpiration.millis - now.millis;

        if (remainingValidMillis / dayInMillis <= 30) {
            // The certificate will expire in the next 30 days.
            warning() << "Server certificate will expire on "
                      << dateToCtimeString(_certExpiration) << " in "
                      << (remainingValidMillis / dayInMillis)
                      << " days.";
        }
    }

}  // namespace mongo
