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

#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

/**
 * Description of actions taken in response to a heartbeat.
 *
 * This includes when to schedule the next heartbeat to a target, and any other actions to
 * take, such as scheduling an election or stepping down as primary.
 */
class HeartbeatResponseAction {
public:
    /**
     * Actions taken based on heartbeat responses
     */
    enum Action {
        NoAction,
        Reconfig,
        StartElection,
        StepDownSelf,
        StepDownRemotePrimary,
        PriorityTakeover
    };

    /**
     * Makes a new action representing doing nothing.
     */
    static HeartbeatResponseAction makeNoAction();

    /**
     * Makes a new action representing the instruction to reconfigure the current node.
     */
    static HeartbeatResponseAction makeReconfigAction();

    /**
     * Makes a new action telling the current node to attempt to elect itself primary.
     */
    static HeartbeatResponseAction makeElectAction();

    /**
     * Makes a new action telling the current node to schedule an event to attempt to elect itself
     * primary after the appropriate priority takeover delay.
     */
    static HeartbeatResponseAction makePriorityTakeoverAction();

    /**
     * Makes a new action telling the current node to step down as primary.
     *
     * It is an error to call this with primaryIndex != the index of the current node.
     */
    static HeartbeatResponseAction makeStepDownSelfAction(int primaryIndex);

    /**
     * Makes a new action telling the current node to ask the specified remote node to step
     * down as primary.
     *
     * It is an error to call this with primaryIndex == the index of the current node.
     */
    static HeartbeatResponseAction makeStepDownRemoteAction(int primaryIndex);

    /**
     * Construct an action with unspecified action and a next heartbeat start date in the
     * past.
     */
    HeartbeatResponseAction();

    /**
     * Sets the date at which the next heartbeat should be scheduled.
     */
    void setNextHeartbeatStartDate(Date_t when);

    /**
     * Gets the action type of this action.
     */
    Action getAction() const {
        return _action;
    }

    /**
     * Gets the time at which the next heartbeat should be scheduled.  If the
     * time is not in the future, the next heartbeat should be scheduled immediately.
     */
    Date_t getNextHeartbeatStartDate() const {
        return _nextHeartbeatStartDate;
    }

    /**
     * If getAction() returns StepDownSelf or StepDownPrimary, this is the index
     * in the current replica set config of the node that ought to step down.
     */
    int getPrimaryConfigIndex() const {
        return _primaryIndex;
    }

private:
    Action _action;
    int _primaryIndex;
    Date_t _nextHeartbeatStartDate;
};

}  // namespace repl
}  // namespace mongo
