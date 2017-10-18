/**
 * Copyright (C) 2017 MongoDB Inc.
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

#pragma once

#include <memory>

namespace mongo {

class ServiceContext;
class SessionsCollection;
class OperationContext;

/**
 * TransactionReaper is responsible for scanning the transaction table, checking if sessions are
 * still alive and deleting the transaction records if their sessions have expired.
 */
class TransactionReaper {
public:
    enum class Type {
        kReplicaSet,
        kSharded,
    };

    virtual ~TransactionReaper() = 0;

    virtual int reap(OperationContext* OperationContext) = 0;

    /**
     * The implementation of the sessions collections is different in replica sets versus sharded
     * clusters, so we have a factory to pick the right impl.
     */
    static std::unique_ptr<TransactionReaper> make(Type type,
                                                   std::shared_ptr<SessionsCollection> collection);
};

}  // namespace mongo
