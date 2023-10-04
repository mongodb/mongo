/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include "mongo/util/cancellation.h"

namespace mongo {
namespace primary_only_service_helpers {

/**
 * Helper class for tracking aborts independently from stepdowns. Intended to be initialized with
 * the cancellation token provided by a PrimaryOnlyService to its run() method, which becomes
 * cancelled when the node is stepping down. Note that since the abortOrStepdown token is derived
 * from the stepdown token, it will be cancelled either when the node steps down or when abort() is
 * called. Users of this class are expected to store some information on their own in order to
 * differentiate between aborts and stepdowns (for example, by writing an abort reason to the state
 * document in the abort case).
 */
class CancelState {
public:
    CancelState(const CancellationToken& stepdownToken);
    const CancellationToken& getStepdownToken() const;
    const CancellationToken& getAbortOrStepdownToken() const;
    bool isSteppingDown() const;
    bool isAbortedOrSteppingDown() const;
    void abort();

private:
    CancellationToken _stepdownToken;
    CancellationSource _abortOrStepdownSource;
    CancellationToken _abortOrStepdownToken;
};

}  // namespace primary_only_service_helpers
}  // namespace mongo
