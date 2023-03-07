# Vector Search in MongoDB Community Version

This is an implementation of vector search built right into the MongoDB binaries. 
Under the hood, it leverages the [Faiss](https://github.com/facebookresearch/faiss) native library
to provide highly performant and scalable search and clustering of dense vectors.

## Create an index

To build an index for similarity vector search, run:

```js
db.runCommand({
    createVectorIndex: '<targetCollectionName>',
    field: '<pathToFieldWithVector>',
    dimensions: <numberOfDimensions>,
    nlist: <numberOfSegments>,
    nprobe: <maxNumberOfSegmentsToSearch>,
})
```

### Command fields

The `createVectorIndex` command takes the following properties:

| Field              | Type     | Description                                                                             |
| ------------------ | -------- | --------------------------------------------------------------------------------------- |
| createVectorIndex  | string   | The name of the target collection where the index is created.                           |
| field              | string   | The path to the field where the vector is stored. For example `prop1.prop2.vector`.     |
| dimensions         | number   | The number of dimensions that each vector has. The default is `4` dimensions.           |
| nlist              | number   | The max number of segments contained by the index.                                      |
| nprobe             | number   | The max number of segments to search. Must be less or equal to `nlist`. Defauls to `1`. |


## Search

To perform a top-k search, you use:

```js
db.<targetCollectionName>.aggregate(
    [
        { $searchVector: { vector: [<inputVector>], field: '<pathToFieldWithVector>', k: 10 } },
    ]
)
```

This example will return the top 10 documents based on the similarity score.

### $searchVector fields

The `$searchVector` stage takes the following properties:

| Field              | Type     | Description                                                   |
| ------------------ | -------- | ------------------------------------------------------------- |
| vector             | array    | The input vector used to perform the search.                  |
| field              | string   | The path to the field where the vector is stored.             |
| k                  | number   | The max number of document matches that stage produces.       |


### Retrieve the similarity score

To retrieve the similarity score, use the `vectorSimilarity` metadata within 
a `$addFields`, and `$meta` stage. For example:

```js
db.<targetCollectionName>.aggregate(
    [
        { $searchVector: { vector: [<inputVector>], field: '<pathToFieldWithVector>', k: 10 } },
        { $addFields: { <fieldNameToAdd>: { $meta: "vectorSimilarity" } } },
    ]
)
```

You can then use the `similarity` field to re-sort the documents if necessary.

## Drop index

To drop a vector index, run:

```js
db.runCommand({
    dropVectorIndex: '<targetCollectionName>',
    field: '<pathToFieldWithVector>',
})
```

### dropVectorIndex fields

The `dropVectorIndex` command takes the following properties:

| Field              | Type     | Description                                                    |
| ------------------ | -------- | -------------------------------------------------------------- |
| dropVectorIndex    | string   | The name of the target collection where the index was created. |
| field              | string   | The path to the field where the vector is stored.              |


## Example

End-to-End example using vector search powered by Faiss.

1. Create a vector index
```js
db.runCommand({
    createVectorIndex: 'demo',
    field: 'vector',
    dimensions: 4,
})
```

2. Insert a document
```js
db.demo.insertOne({
    name: 'Sample vector',
    vector: [0.001, 0.001, 0.001, 0.001],
})
```

3. Search
```js
db.demo.aggregate(
    [
        { $searchVector: { vector: [0.001, 0.001, 0.001, 0.001], field: 'vector', k: 10 } },
        { $addFields: { similarity: { $meta: "vectorSimilarity" } } },
    ]
)
```
