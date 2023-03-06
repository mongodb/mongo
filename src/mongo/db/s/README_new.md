
> **Warning**
> This is work in progress and some sections are incomplete

# Sharding Architecture Guide
This page contains details of the code architecture of the MongoDB Sharding system. It is intended to be used by engineers on the core server, with some sections being more appropriate for sharding engineers.

It is not intended to be a tutorial on how to operate sharding as a user and it requires that the reader is already familiar with the general concepts of [sharding](https://docs.mongodb.com/manual/sharding/#sharding), the
[architecture of a MongoDB sharded cluster](https://docs.mongodb.com/manual/sharding/#sharded-cluster),
and the concept of a [shard key](https://docs.mongodb.com/manual/sharding/#shard-keys).

## Sharding terminology and acronyms

TODO: 

## Table of contents

TODO: 

## Sharding code architectural diagram

This section visualises the architecture of the Sharding system and contains links to the sections which describe the various subsystems.

```mermaid
C4Context
  Container_Boundary(Sharding, "Sharding") {
    Container_Boundary(ShardingCatalogAPI, "Sharding Catalog API") {
      Component(ShardRole, "Shard Role")
      Component(RouterRole, "Router Role")
    }
    Container_Boundary(ShardingCatalogRuntime, "Sharding Catalog Runtime") {
      Container_Boundary(CatalogContainers, "Catalog Containers") {
        ContainerDb(MDBCatalog, "__mdb_catalog")
        ContainerDb(CSRSCollections, "CSRS Collections")
        ContainerDb(ShardLocalCollections, "Shard Local Collections")
        ContainerDb(ShardLocalCollections, "Shard Local Collections")
      }
      Container_Boundary(CatalogContainerCaches, "Catalog Container Caches") {
        Component(CatalogCache, "Catalog Cache")
        Component(SSCache, "Sharding State Cache")
        Component(DSSCache, "Database Sharding State Cache")
        Component(CSSCache, "Collection Sharding State Cache")
      }
      
      Component(DDLCoordinators, "DDL Coordinators")
    }
  }
```
