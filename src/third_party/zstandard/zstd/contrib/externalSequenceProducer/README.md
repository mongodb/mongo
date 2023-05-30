externalSequenceProducer
=====================

`externalSequenceProducer` is a test tool for the Block-Level Sequence Producer API.
It demonstrates how to use the API to perform a simple round-trip test.

A sample sequence producer is provided in sequence_producer.c, but the user can swap
this out with a different one if desired. The sample sequence producer implements
LZ parsing with a 1KB hashtable. Dictionary-based parsing is not currently supported.

Command line :
```
externalSequenceProducer filename
```
