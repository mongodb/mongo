> **Warning**
> This is work in progress and some sections are incomplete

# Sharding Architecture Guide
This page contains details of the source code architecture of the MongoDB Sharding system. It is intended to be used by engineers working on the core server, with some sections containing low-level details which are most appropriate for new engineers on the sharding team.

It is not intended to be a tutorial on how to operate sharding as a user and it requires that the reader is already familiar with the general concepts of [sharding](https://docs.mongodb.com/manual/sharding/#sharding), the [architecture of a MongoDB sharded cluster](https://docs.mongodb.com/manual/sharding/#sharded-cluster), and the concept of a [shard key](https://docs.mongodb.com/manual/sharding/#shard-keys).

## Sharding terminology and acronyms
TODO: 

## Table of contents
TODO: 

## Sharding code architectural diagram

This section visualises the architecture of the MongoDB Sharding system along with links to sections in this architecture guide which describe the various subsystems in more detail.

```mermaid
C4Component

Container_Boundary(Sharding, "Sharding", $link="README_new.md") {
  Container_Boundary(ShardingCatalogAPI, "Sharding Catalog API", $link="README_sharding_catalog.md") {
    Component(RouterRole, "Router Role", $link="README_sharding_catalog.md#router-role")
    Component(ShardRole, "Shard Role", $link="README_sharding_catalog.md#shard-role")
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
