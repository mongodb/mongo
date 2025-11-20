# Flow Control

The Flow Control mechanism aims to keep replica set majority committed lag less than or equal to a
configured maximum. The default value for this maximum lag is 10 seconds. The Flow Control mechanism
starts throttling writes on the primary once the majority committed replication lag reaches a
threshold percentage of the configured maximum. The mechanism uses a "ticket admission"-based
approach to throttle writes. With this mechanism, in a given period of 1 second, a fixed number of
"flow control tickets" is available. Operations must acquire a flow control ticket in order to
acquire a global IX lock to execute a write. Acquisition attempts that occur after this fixed number
has been granted will stall until the next 1 second period. Certain system operations circumvent the
ticket admission mechanism and are allowed to proceed even when there are no tickets available.

To address the possibility of this Flow Control mechanism causing indefinite stalls in
Primary-Secondary-Arbiter replica sets in which a majority cannot be established, the mechanism only
executes when read concern majority is enabled. Additionally, the mechanism can be disabled by an
admin.

Flow Control is configurable via several server parameters. Additionally, currentOp, serverStatus,
database profiling, and slow op log lines include Flow Control information.

## Flow Control Ticket Admission Mechanism

The ticket admission Flow Control mechanism allows a specified number of global IX lock acquisitions
every second. Most global IX lock acquisitions (except for those that explicitly circumvent Flow
Control) must first acquire a "Flow Control ticket" before acquiring a ticket for the lock. When
there are no more flow control tickets available in a one second period, remaining attempts to
acquire flow control tickets stall until the next period, when the available flow control tickets
are replenished. It should be noted that there is no "pool" of flow control tickets that threads
give and take from; an independent mechanism refreshes the ticket counts every second.

When the Flow Control mechanism refreshes available tickets, it calculates how many tickets it
should allow in order to address the majority committed lag.

The Flow Control mechanism determines how many flow control tickets to replenish every period based
on:

1. The current majority committed replication lag with respect to the configured target maximum
   replication lag
1. How many operations the secondary sustaining the commit point has applied in the last period
1. How many IX locks per operation were acquired in the last period

## Configurable constants

Criterion #2 determines a "base" number of flow control tickets to be used in the calculation. When
the current majority committed lag is greater than or equal to a certain configurable threshold
percentage of the target maximum, the Flow Control mechanism scales down this "base" number based on
the discrepancy between the two lag values. For some configurable constant 0 < k < 1, it calculates
the following:

`base * k ^ ((lag - threshold)/threshold) * fudge factor`

The fudge factor is also configurable and should be close to 1. Its purpose is to assign slightly
lower than the "base" number of flow control tickets when the current lag is close to the threshold.
Criterion #3 is then multiplied by the result of the above calculation to translate a count of
operations into a count of lock acquisitions.

When the majority committed lag is less than the threshold percentage of the target maximum, the
number of tickets assigned in the previous period is used as the "base" of the calculation. This
number is added to a configurable constant (the ticket "adder" constant), and the sum is multiplied
by another configurable constant (the ticket "multiplier" constant). This product is the new number
of tickets to be assigned in the next period.

When the Flow Control mechanism is disabled, the ticket refresher mechanism always allows one
billion flow control ticket acquisitions per second. The Flow Control mechanism can be disabled via
a server parameter. Additionally, the mechanism is disabled on nodes that cannot accept writes.

Criteria #2 and #3 are determined using a sampling mechanism that periodically stores the necessary
data as primaries process writes. The sampling mechanism executes regardless of whether Flow Control
is enabled.

## Oscillations

There are known scenarios in which the Flow Control mechanism causes write throughput to
oscillate. There is no known work that can be done to eliminate oscillations entirely for this
mechanism without hindering other aspects of the mechanism. Work was done (see SERVER-39867) to
dampen the oscillations at the expense of throughput.

## Throttling internal operations

The Flow Control mechanism throttles all IX lock acquisitions regardless of whether they are from
client or system operations unless they are part of an operation that is explicitly excluded from
Flow Control. Writes that occur as part of replica set elections in particular are excluded. See
SERVER-39868 for more details.
