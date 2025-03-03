# EloqDoc  
A MongoDB-compatible, high-performance, elastic, distributed document database.

[![GitHub Stars](https://img.shields.io/github/stars/eloqdata/eloqdoc?style=social)](https://github.com/eloqdata/eloqdoc/stargazers)
---

## Overview
EloqDoc is a high-performance, elastic, distributed transactional document database with MongoDB compability. Built on top of [Data Substrate](https://www.eloqdata.com/blog/2024/08/11/data-substrate), it leverages a decoupled storage and compute architecture to deliver fast scaling, ACID transaction support, and efficient resource utilization.

EloqDoc eliminates the need for sharding components like `mongos` in MongoDB, offering a simpler, more powerful distributed database experience. It’s ideal for workloads requiring rapid scaling, high write throughput, and flexible resource management.

Explore [EloqDoc](https://www.eloqdata.com/product/eloqdoc) website for more details.

👉 **Use Cases**: web applications, ducument store, content management systems — anywhere you need MongoDB compatibility **but** demand distributed performance and elasticity.

---

## Key Features

### ⚙️ MongoDB Compatibility
Seamlessly integrates with MongoDB clients, drivers, and tools, enabling you to use existing MongoDB workflows with a distributed backend.

### 🌐 Distributed Architecture
Supports **multiple writers** and **fast distributed transactions**, ensuring high concurrency and fault tolerance across a cluster without sharding complexity.

### 🔄 Elastic Scalability
- Scales compute and memory **100x faster** than traditional databases by avoiding data movement on disk.
- Scales storage independently, conserving CPU resources for compute-intensive tasks.
- Scales redo logs independently to optimize write throughput.

### 🔥 High-Performance Transactions
Delivers **ACID transaction support** with especially fast distributed transactions, making it suitable for mission-critical applications.

### 🔒 Simplified Distributed Design
Operates as a distributed database without requiring a sharding coordinator (e.g., `mongos`), reducing operational complexity and overhead.

---

## Architecture Highlights

- **Fast Scaling**: Compute and memory scale independently without disk data movement, enabling rapid elasticity for dynamic workloads.
- **Storage Flexibility**: Storage scales separately from compute, optimizing resource allocation and reducing waste.
- **Write Optimization**: Independent redo log scaling boosts write throughput, ideal for high-velocity data ingestion.
- **No Sharding Overhead**: Distributes data natively across the cluster, eliminating the need for additional sharding components.

---

## Build from Source

Follow these steps to build and run EloqDoc from source.

### 1. Initialize Submodules
Fetch dependencies:

```
git submodule update --init --recursive
```

### 2. Install Dependencies
Install required build dependencies (Ubuntu 20.04 example):

```bash
bash scripts/install_dependency_ubuntu2004.sh
```

### 3. Build EloqDoc
Configure and compile with optimized settings:

```
mkdir build
cd build
env 'CXXFLAGS=-Wno-error -fPIC' cmake -G 'Unix Makefiles' -S src/mongo/db/modules/eloq -B src/mongo/db/modules/eloq/build -DCMAKE_INSTALL_PREFIX=/home/ubuntu/mongo/install -DCMAKE_CXX_STANDARD=17 '-DCMAKE_CXX_FLAGS_DEBUG_INIT=-Wno-error -fPIC' -DCMAKE_BUILD_TYPE=Debug -DRANGE_PARTITION_ENABLED=ON -DCOROUTINE_ENABLED=ON -DEXT_TX_PROC_ENABLED=ON -DWITH_LOG_SERVICE=ON -DBUILD_WITH_TESTS=ON -DSTATISTICS=ON -DUSE_ASAN=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=1
cmake --build src/mongo/db/modules/eloq/build --config Debug -j8
cmake --install src/mongo/db/modules/eloq/build --config Debug

python buildscripts/scons.py MONGO_VERSION=4.0.3  \
    VARIANT_DIR=Debug \
    LIBPATH=/usr/local/lib \
    CXXFLAGS="-Wno-nonnull -Wno-class-memaccess -Wno-interference-size -Wno-redundant-move" \
    --build-dir=#build \
    --prefix=/home/ubuntu/mongo/install \
    --dbg=on \
    --opt=off \
    --allocator=system \
    --link-model=dynamic \
    --install-mode=hygienic \
    --disable-warnings-as-errors \
    -j8 \
    install-core
```

### 4. Set Up Storage Backend
EloqDoc supports multiple storage backends (e.g., Cassandra). Example setup with Cassandra:

```bash
wget https://archive.apache.org/dist/cassandra/4.1.8/apache-cassandra-4.1.8-bin.tar.gz
tar -zxvf apache-cassandra-4.1.8-bin.tar.gz
./apache-cassandra-4.1.8/bin/cassandra -f
# Verify Cassandra is running:
./apache-cassandra-4.1.8/bin/cqlsh localhost -u cassandra -p cassandra
```

### 5. Config EloqDoc
Edit eloqdoc-config.cnf with example settings:
```
systemLog:
  verbosity: 2
  destination: file
  path: "/home/ubuntu/mongo/install/log/mongod.log"
  component:
    ftdc:
      verbosity: 0
net:
  port: 27017
  serviceExecutor: "adaptive"
  enableCoroutine: true
  reservedThreadNum: 2
  adaptiveThreadNum: 1
storage:
  dbPath: "/home/ubuntu/mongo/install/data"
  engine: "eloq"
  eloq:
    txService:
      coreNum: 2
      checkpointerIntervalSec: 10
      nodeMemoryLimitMB: 8192
      nodeLogLimitMB: 8192
      realtimeSampling: true
      collectActiveTxTsIntervalSec: 2
      checkpointerDelaySec: 5
    storage:
      keyspaceName: "mongo_test"
      cassHosts: 127.0.0.1
setParameter:
  diagnosticDataCollectionEnabled: false
  disableLogicalSessionCacheRefresh: true
  ttlMonitorEnabled: false
```

### 6. Start EloqDoc Node

```bash
export LD_PRELOAD=/usr/local/lib/libmimalloc.so:/lib/libbrpc.so
./install/bin/mongod --config scripts/eloqdoc-config.cnf
```

### 7. Connect to EloqSQL
```bash
./install/bin/mongo --eval "db.runCommand({ping: 1})"
```

---

**Star This Repo ⭐** to Support Our Journey — Every Star Helps Us Reach More Developers!
