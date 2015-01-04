/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#pragma once

#include <boost/scoped_ptr.hpp>

#include "mongo/db/operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker_noop.h"
#include "mongo/db/curop.h"
#include "mongo/db/storage/recovery_unit_noop.h"

namespace mongo {

    class OperationContextNoop : public OperationContext {
    public:
        OperationContextNoop(RecoveryUnit* ru)
            : _recoveryUnit(ru),
              _locker(new LockerNoop()) {

        }

        OperationContextNoop()
            : _recoveryUnit(new RecoveryUnitNoop()),
              _locker(new LockerNoop()) {

        }

        virtual ~OperationContextNoop() { }

        virtual Client* getClient() const {
            invariant(false);
            return NULL;
        }

        virtual CurOp* getCurOp() const {
            invariant(false);
            return NULL;
        }

        virtual RecoveryUnit* recoveryUnit() const {
            return _recoveryUnit.get();
        }

        virtual RecoveryUnit* releaseRecoveryUnit() {
            return _recoveryUnit.release();
        }

        virtual void setRecoveryUnit(RecoveryUnit* unit) {
            _recoveryUnit.reset(unit);
        }

        virtual Locker* lockState() const {
            return _locker.get();
        }

        virtual ProgressMeter* setMessage(const char * msg,
                                          const std::string &name,
                                          unsigned long long progressMeterTotal,
                                          int secondsBetween) {
            return &_pm;
        }

        virtual void checkForInterrupt() const { }
        virtual Status checkForInterruptNoAssert() const {
            return Status::OK();
        }

        virtual bool isPrimaryFor( const StringData& ns ) {
            return true;
        }

        virtual bool isGod() const {
            return false;
        }

        virtual string getNS() const {
            return string();
        };

        virtual unsigned int getOpID() const {
            return 0;
        }

    private:
        std::auto_ptr<RecoveryUnit> _recoveryUnit;
        boost::scoped_ptr<Locker> _locker;
        ProgressMeter _pm;
    };

}  // namespace mongo
