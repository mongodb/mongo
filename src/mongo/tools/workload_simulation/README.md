# Workload Simulation

This directory provides code for generating `*_simulator` executables that test various components
of the system under a simulated workload. The executables produces log output that can be converted
into graphs plotting metrics collected over time using `process_logs.py`.

The simulations themselves are carried out using an event queue and a mock clock. Workload drivers
spawn and manage actor threads that are responsible for sending _event_ requests to the queue,
where each request is tied to a time. The queue is responsible for advancing the clock
appropriately and notifying the actor threads when the clock has reached the appropriate time for
their requests to be processed. Additional, non-actor threads can similarly wait for specific times
during the simulation by registering _observer_ requests that are maintained in a separate internal
queue.

More specifically:

1. The event queue waits until it has received an _event_ request from each reported actor thread.
2. When it is sufficiently full, the queue determines the minimum time of a queued _event_
   request, and advances the mock clock to that time.
3. The queue then notifies any threads waiting on either _event_ or _observer_ requests at or
   before the current time.

The _observer_ mechanism is used to trigger periodic metric reporting for the log output. It may
also be used by workload drivers to trigger periodic jobs.

## Defining a Simulation

The [`simulation.h`](simulation.h) header defines a `SIMULATION` macro that functions similarly to
the `TEST_F` macro in our unittesting framework. It expects the first parameter to be the name of
a fixture class derived from `mongo::workload_simulation::Simulation`. The second parameter is the
name of the workload, and any additional parameters will be passed as input to the constructor of
the fixture class.

A C++ file that defines these macros can be compiled using the scons helper `WorkloadSimulator` to
link the relevant `main` implementation that runs all the workloads defined using the macro. The
name of the target _must_ end in `_simulator`.

## Generating Visualizations

A `*_simulator` executable simply runs all selected workloads and outputs logs to stdout.
These logs can be piped to `process_logs.py` to generate graphs. Ex.

```
$ throughput_probing_simulator | process_logs.py -o ~/sim_output
```
