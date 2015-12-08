/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#pragma once

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/jsobj.h"

namespace boost {
namespace filesystem {
class path;
}  // namespace filesystem
}  // namespace boost

namespace mongo {
class DataProtector;
class StringData;
class ServiceContext;

class WiredTigerCustomizationHooks {
public:
    static void set(ServiceContext* service,
                    std::unique_ptr<WiredTigerCustomizationHooks> custHooks);

    static WiredTigerCustomizationHooks* get(ServiceContext* service);

    virtual ~WiredTigerCustomizationHooks() = default;

    /**
     * Returns true if the customization hooks are enabled.
     */
    virtual bool enabled() const = 0;

    /**
     * Perform any encryption engine initialization/sanity checking that needs to happen after
     * storage engine initialization but before the server starts accepting incoming connections.
     *
     * Returns true if the server needs to be rebooted because of configuration changes.
     */
    virtual bool restartRequired() = 0;

    /**
     *  Gets the WiredTiger encryption configuration string for the
     *  provided table name
     */
    virtual std::string getOpenConfig(StringData tableName) = 0;

    /**
     * Returns the maximum size addition when doing transforming temp data.
     */
    size_t additionalBytesForProtectedBuffer() {
        return 33;
    }

    /**
     * Get the data protector object
     */
    virtual std::unique_ptr<DataProtector> getDataProtector() = 0;

    /**
     * Get an implementation specific path suffix to tag files with
     */
    virtual boost::filesystem::path getProtectedPathSuffix() = 0;

    /**
     * Transform temp data to non-readable form before writing it to disk.
     */
    virtual Status protectTmpData(
        const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, size_t* resultLen) = 0;

    /**
     * Tranforms temp data back to readable form, after reading from disk.
     */
    virtual Status unprotectTmpData(
        const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, size_t* resultLen) = 0;
};

// Empty default implementation of the abstract class WiredTigerCustomizationHooks
class EmptyWiredTigerCustomizationHooks : public WiredTigerCustomizationHooks {
public:
    ~EmptyWiredTigerCustomizationHooks() override;

    bool enabled() const override;

    bool restartRequired() override;

    std::string getOpenConfig(StringData tableName) override;

    std::unique_ptr<DataProtector> getDataProtector() override;

    boost::filesystem::path getProtectedPathSuffix() override;

    Status protectTmpData(
        const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, size_t* resultLen) override;

    Status unprotectTmpData(
        const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, size_t* resultLen) override;
};
}  // namespace mongo
