# Eviction

### Overview

The Eviction module ensures that the amount of data kept in WiredTiger's cache is efficiently managed to stay within user-defined boundaries for optimal performance. These boundaries are set defined by the **target<sup>1</sup>** and **trigger<sup>2</sup> thresholds** for total<sup>3</sup>, dirty<sup>4</sup>, and updates<sup>5</sup> content. When the cache exceeds these limits, eviction processes begin to evict the content from cache and write it to disk.

### Key Parameters

WiredTiger offers multiple eviction configuration options in [api_data.py](../../dist/api_data.py). These settings can be adjusted during database opening (`wiredtiger_open`) or reconfigured later (`WT_CONNECTION::reconfigure`). The most important configuration options for eviction are:

| Parameter               | Description                                                                                              |
| ----------------------- | -------------------------------------------------------------------------------------------------------- |
| **`eviction_target`**    | The target<sup>1</sup> percentage of cache usage that eviction tries to maintain.                                  |
| **`eviction_trigger`**   | When cache usage exceeds this threshold, **application threads**  start assisting in eviction.     |
| **`eviction_dirty_target`** | The target<sup>1</sup> percentage of **dirty<sup>4</sup>** cache usage that eviction tries to maintain.                      |
| **`eviction_dirty_trigger`** | When cache usage exceeds this **dirty<sup>4</sup>** threshold, **application threads**  start assisting in eviction.   |
| **`eviction_updates_target`** | The target<sup>1</sup> percentage of **updates<sup>5</sup>** cache usage that eviction tries to maintain.                 |
| **`eviction_updates_trigger`** | When cache usage exceeds this **updates<sup>5</sup>** threshold, **application threads** start assisting in eviction.                   |

> **Note:** Target<sup>1</sup> sizes must always be lower than their corresponding trigger<sup>2</sup> sizes.

### Eviction Process

The eviction process involves three components:

- **Eviction Server**: The `eviction server` thread identifies evictable pages/candidates, places them in eviction queues and sorts them based on the **Least Recently Used (LRU)** algorithm. It is a background process that commences when any **target<sup>1</sup> threshold** is reached.
- **Eviction Worker Threads**: These background threads pop pages from the eviction queues, write the page to disk, and then remove it from the cache. The `threads_max` and `threads_min` configurations in [api_data.py](../../dist/api_data.py) control the maximum and minimum number of eviction worker threads in WiredTiger.
    > It is possible to run only the eviction server without the eviction worker threads, but this may result in slower eviction as the server thread would also be responsible for evicting pages in the eviction queues.
- **Application Threads Eviction**: 
    - When background eviction threads are unable to maintain cache content and cache content exceeds **trigger<sup>2</sup> thresholds**, WiredTiger will pause application threads to have them assist the eviction worker threads evicting pages in the eviction queues.
    - Another scenario, known as **forced eviction**, occurs when application threads directly evict pages that meet the criteria for immediate eviction. Some example criteria include:
        - Pages exceeding the configured `memory_page_max` size (defined in [api_data.py](../../dist/api_data.py))
        - Pages with large skip list
        - Empty Internal pages
        - Obsolete pages
        - Pages with long update chains
        - Pages showing many deleted records
    > In both cases described above, application threads may experience higher read/write latencies.

### APIs for Eviction

The eviction APIs, declared in `evict.h`, allow other modules in WT to manage eviction processes. Below is a brief description of the functionalities provided by these APIs:
- Pausing and resuming the eviction server when necessary.
- Specifying which Btrees to prioritize or exclude from the eviction process.
- Monitoring eviction health, including eviction lag, slow progress, and stalled progress.
- Allowing callers to evict pages or entire Btrees directly, bypassing the background eviction process.
- Modifying page states, crucial for prioritizing or deprioritizing pages for eviction.
> For a detailed explanation of API functionality (declared in `evict.h`), refer to the comments located above each function definition.

### Terminology

- <sup>1</sup>target: The level of cache usage eviction tries to maintain.
- <sup>2</sup>trigger: The level of cache usage at which application threads assist with the eviction of pages.
- <sup>3</sup>total: The combined memory usage of clean pages, dirty pages, and in-memory updates.
- <sup>4</sup>dirty: The memory usage of all the modified pages that have not yet been written to disk.
- <sup>5</sup>updates: The memory usage of all the in-memory updates. After a checkpoint pages may still contain modifications that have been persisted to disk. This memory is not clean - as it's not an exact replica of the disk image - but it's also not dirty as the data is persisted on disk. We track this data as "updates"
