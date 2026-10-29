// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once
#include "mongo/util/modules.h"
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
        StepDownSelf,
        PriorityTakeover,
        CatchupTakeover,
        RetryReconfig,
        RestartHeartbeats
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
     * Makes a new action telling the current node to schedule an event to attempt to elect itself
     * primary after the appropriate priority takeover delay.
     */
    static HeartbeatResponseAction makePriorityTakeoverAction();

    /**
     * Makes a new action telling the current node to schedule an event to attempt to elect itself
     * primary after the appropriate catchup takeover delay.
     */
    static HeartbeatResponseAction makeCatchupTakeoverAction();

    /**
     * Makes a new action telling the current node to attempt to find itself in its current replica
     * set config again, in case the previous attempt's failure was due to a temporary DNS outage.
     */
    static HeartbeatResponseAction makeRetryReconfigAction();

    /**
     * Makes a new action telling the current node to step down as primary.
     *
     * It is an error to call this with primaryIndex != the index of the current node.
     */
    static HeartbeatResponseAction makeStepDownSelfAction(int primaryIndex);

    /**
     * Makes a new action telling the current node to cancel and restart all heartbeats. This should
     * be very rare and only used if the `disableReplicationUsageOfPriorityPort` parameter is
     * modified.
     */
    static HeartbeatResponseAction makeRestartHeartbeatsAction();

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
     * Sets whether or not the member's opTime has advanced or config has changed since the
     * last heartbeat response.
     */
    void setAdvancedOpTimeOrUpdatedConfig(bool advancedOrUpdated);

    /*
     * Sets whether or not the member has transitioned from unelectable to electable since the last
     * heartbeat response.
     */
    void setBecameElectable(bool becameElectable);

    /*
     * Sets whether or not the member has changed member state since the last heartbeat response.
     */
    void setChangedMemberState(bool changedMemberState);

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

    /*
     * Returns true if the heartbeat response results in the conception of the
     * member's optime moving forward or the member's config being newer.
     */
    bool getAdvancedOpTimeOrUpdatedConfig() const {
        return _advancedOpTimeOrUpdatedConfig;
    }

    /*
     * Returns true if the heartbeat response results in the member transitioning from unelectable
     * to electable.
     */
    bool getBecameElectable() const {
        return _becameElectable;
    }

    /*
     * Returns true if the heartbeat response results in the member changing member state.
     */
    bool getChangedMemberState() const {
        return _changedMemberState;
    }

    /*
     * Returns true if the heartbeat results in any significant change in member data.
     */
    bool getChangedSignificantly() const {
        return _changedMemberState || _advancedOpTimeOrUpdatedConfig || _becameElectable;
    }

private:
    Action _action;
    int _primaryIndex;
    Date_t _nextHeartbeatStartDate;
    bool _advancedOpTimeOrUpdatedConfig = false;
    bool _becameElectable = false;
    bool _changedMemberState = false;
};

}  // namespace repl
}  // namespace mongo
