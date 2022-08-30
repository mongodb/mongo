# Free Monitoring

## Table of Contents

- [Free Monitoring](#free-monitoring)
  - [Table of Contents](#table-of-contents)
  - [High Level Overview](#high-level-overview)

## High Level Overview

Free Monitoring is a way for MongoDB Community users to enable Cloud Monitoring on their database.
To use Free Monitoring, a customer must first register by issuing the command
`db.enableFreeMonitoring()`.

The entire Free Monitoring subsystem is controlled by an object of type
[`FreeMonController`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_controller.h#L53).
The `FreeMonController` lives as a decoration on the `ServiceContext`. The `FreeMonController` has a
two collections of collectors - the
[`_registrationCollectors`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_controller.h#L197)
which collect data at registration time, and the
[`_metricCollectors`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_controller.h#L200)
which collect data periodically. It also owns a
[`FreeMonProcessor`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.h#L304)
which under the hood contains a multi-producer priority queue, and a
[`FreeMonNetworkInterface`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_network.h#L40)
which is a way for the subsystem to send and receive packets to the cloud endpoint.

When the server first starts, if Free Monitoring is enabled (using a command line parameter
`enableFreeMonitoring`), the FreeMonController is initialized on server startup through the mongod
main function which calls
[`startFreeMonitoring`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_mongod.cpp#L310).
This function creates the
[`FreeMonNetworkInterface`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_mongod.cpp#L322),
initializes the controller, determines the Registration type, and calls
[`start`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_mongod.cpp#L346-L348)
on the controller. The
[`start`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_controller.h#L59-L65)
function initializes the processor, creates a thread for it to run on, and performs registration if
the user has performed registration.

If the user has not performed registration, the metrics collector begins collecting data. It stores
this data in a MetricsBuffer, capable of holding up to 10 data points. When the user performs
registration, a call to
[`registerServerStartup`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_controller.cpp#L79-L84)
is made, placing a
[`FreeMonMessageWithPayload`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_message.h#L255)
object in the processor's queue. A `FreeMonMessageWithPayload` is an expanded subclass of
[`FreeMonMessage`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_message.h#L146),
which represents a message sent to the
[`FreeMonProcessor`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.h#L304)
actor to process and make a decision. For more reading on the actor model that the Free Monitoring
system is based on, see [here](https://en.wikipedia.org/wiki/Actor_model). A `FreeMonMessage` has
two significant properties, a type and a deadline. The type determines how the queue responds when
processing the message. The code for how the queue processes messages can be found
[here](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L156-L269).
The deadline determines the priority of the message in the queue. The deadline represents both the
waiting period the queue must take to process a message and a priority the queue uses to determine
the order in which messages are processed. For example, a message with a deadline of now can be
processed before something with a deadline of an hour from now.

In the call to
[`registerServerStartup`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_controller.cpp#L79-L84),
a message of type `RegisterServer` is sent to the queue. Included with the message is a payload of
the registration type that the server should perform. The queue processes this message and calls
[`doServerRegister`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L324)
with the message object. The function processes the `RegistrationType` and if it is
`RegisterOnStart`, sends a `RegisterCommand` message to the queue. If the `RegistrationType` is
something else, we try to determine whether we are a primary or secondary in a replica set and send
a message depending on the state of free monitoring in the set. The full logic with comments is
[here](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L330-L367),
but know that it may create a `RegisterCommand` message to the queue in certain cases. The function
[`doServerRegister`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L328)
also creates a message of type `MetricsCollect` in the queue.

When the queue processes the
[`RegisterCommand`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L173-L175)
message, it calls
[`doCommandRegister`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L406)
which uses the
[`FreeMonNetworkInterface`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_mongod.cpp#L322)
to send a message over the wire to the cloud endpoint. It also writes the registration state
(`FreeMonRegistrationStatus::kPending`) and registration information out to disk, calling
[`writeState`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L299),
which invokes functions on the
[`FreeMonStorage`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_storage.h#L43)
class. The job of the `FreeMonStorage` class is to provide an interface for different functions to
interact with the storage subsystem. Whenever a change is made to the registration state -
registration completes or registration is cancelled because of an endpoint error - the processor
writes this information out to disk.

The queue also processes the first `MetricsCollect` command around this time. The queue reads the
Metrics collect method and calls
[`doMetricsCollect`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L714-L726)
which fires the `_metrics` collectors to collect and stores the data in a
[`MetricsBuffer`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.h#L185-L225).
The buffer can hold 10 data points at a time, so if the data has not been synced to the cloud
endpoint by the time the 11th data point is collected, then the buffer will remove the last item
from the queue. The function
[`doMetricsCollect`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L724-L725)
creates another message of type `MetricsCollect` with a deadline of the specified
`_metricsGatherInterval` for collection.

The way for a queue to trigger sending new metrics to the server is by sending a message of type
`MetricsSend`. This occurs on a few occasions - when registration information has been successfully
[sent to the cloud
endpoint](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L666),
when metrics information has [successfully been sent to the cloud
endpoint](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L865-L866),
and when metrics information has [failed to send to the
endpoint](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.cpp#L886-L888).
In the first case, a message is created with a deadline of now. In the second case the message is
sent with a deadline that is tracked by the
[`MetricsRetryCounter`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_processor.h#L153-L183)
object. The retry object is used to track any failures the processor encountered when sending
metrics; if enough failures have occured in a row, then the processor stops sending the metrics. In
the third case, the `MetricsRetryCounter` object is incremented to indicate failure. If it has not
exceeded the retry limit, then the message is again sent with a deadline tracked by the retry
object.

The last notable part of `FreeMonitoring` is the
[`FreeMonOpObserver`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_op_observer.h#L40).
This `OpObserver` watches the namespace where the free monitoring registration information is stored
(the `admin` database and the `system.version` collection) and sends a message to the processor if
there is a change to the document for the registration information. For example, if someone updates
the document, the OpObserver calls
[`notifyOnUpsert`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_controller.h#L133-L138)
in the controller, the controller queues a
[`NotifyOnUpsert`](https://github.com/mongodb/mongo/blob/r4.4.0/src/mongo/db/free_mon/free_mon_controller.cpp#L111)
command in the processor. When the processor reads that message, it reads the updated registration
state from disk and updates the free monitoring subsystem based on the new information.
