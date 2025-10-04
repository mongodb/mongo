# Checkpoint

## Overview

Checkpoint is responsible for ensuring that all data is durable at a point in time. WiredTiger can
recover from this point in the event of an unexpected shutdown or crash. A checkpoint is performed
within the context of snapshot isolation transaction as such the checkpoint has a consistent view of
the database from beginning to end. See [arch-checkpoint.dox](../docs/arch-checkpoint.dox#10) for
more information on snapshot isolation and timestamps. The checkpoint operation deals with data in
memory and on disk and is therefore not a self-contained module. The responsibilities are shared
across the btree, block, metadata, and checkpoint modules. The checkpoint module directory is an
entry point for all other parts of the checkpoint operation. The role of the other modules during
checkpoint can briefly be described as:

### [bt_sync.c](../btree/bt_sync.c)

- Walk the btree and flush all dirty pages to disk.

### [block_ckpt.c](../block/block_ckpt.c)

- Manage the checkpoint extent lists. See [arch-block.dox](../docs/arch-block.dox#208), for more
  information on checkpoint extent list merging.
- Write the extent list blocks to disk.

### [meta_ckpt.c](../meta/meta_ckpt.c)

- Track each files respective checkpoints in the metadata file. Used for startup and recovery.

## Key Parameters

The checkpoint API contains multiple configuration options in [api_data.py](../../dist/api_data.py).
We highlight the most important configuration options here:

| Parameter                  | Description            |
| -------------------------- | ---------------------- |
| **`force`**                      | By default a file may be skipped if the underlying object has not been modified. If true, every file will be forced to take a checkpoint. |
| **`name`**                       | Specify a name for the checkpoint. |
| **`use_timestamp`**              | If true (the default), create the checkpoint as of the last stable timestamp if timestamps are in use, or with all committed updates if there is no stable timestamp set. If false, always generate a checkpoint with all committed updates, ignoring any stable timestamp |

> **Note:** **`eviction_checkpoint_target`** is another important parameter in the eviction module.
> It is intended to leverage the multithreaded behaviour of the eviction server to write out dirty
> pages before proceeding with checkpoint

## Checkpoint operation

The checkpoint operation goes through five stages:

- Prepare
- Data files checkpoint
- History store checkpoint
- Flush to disk
- Metadata checkpoint

Each stage is described in detail in [arch-checkpoint.dox](../docs/arch-checkpoint.dox).

## APIs for Checkpoint

The checkpoint APIs are declared in [checkpoint.h](./checkpoint.h). Below is a brief description of
the functionalities provided by these APIs:

- List the files to checkpoint.
- Take a checkpoint of a file. It is worth noting that there is a distinct API when taking a
  checkpoint before closing a file.
- Cleanup checkpoint-related structures.
- Log checkpoint progress messages.

There are also APIs dedicated to the checkpoint server to perform the following:

- Create and destroy the thread.
- Signal the thread to start a checkpoint.

> For more information on each API, refer to the comments located above each function definition in
> [checkpoint.h](./checkpoint.h).
