# Throughput Probing Simulations

This directory provides code for an executable `throughput_probing_simulator` that tests our
throughput probing algorithm used for execution control under a simulated workload. The workload
output consists of graphs showing the actual and optimal ticket allocations over time.

The rough workflow for an individual simulation is:

1. Specify an optimal concurrency level, and the throughput at that concurrency level.
2. Provide a well-behaved model for what throughput we will observe at different concurrency
   levels, and what operation latencies would produce that throughput.
3. Use that model to specify a workload driver which simulates operations with the given
   latencies based on the current concurrency level selected by the throughput probing
   algorithm.
4. Run the mock workload for specified duration.

## File Structure

New workloads can be added in [`workloads.cpp`](workloads.cpp)

The existing `TicketedWorkloadDriver` is quite flexible, and should suffice for most workloads by
simply tuning the workload characteristics, runtime, etc. New workload characterstic types can be
defined in [`workload_characteristics.h`](../workload_characteristics.h).
