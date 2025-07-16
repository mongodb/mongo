# Host API

## DocumentSourceExtension

Aggregation stages are modelled and implemented within the server as `DocumentSource`
specializations. The Host API introduces `DocumentSourceExtension`, a specialization of the
`DocumentSource` abstraction, which acts as a façade and delegates its business logic to the
Extensions API.

All the methods offered by the `DocumentSource` interface are implemented by modelling a
comprehensive set of interfaces in the API which the extension must service.

`DocumentSourceExtension` delegates calls to its interface to the following Public API components:

- `MongoExtensionAggregationStageDescriptor` (Static Descriptor)
- `MongoExtensionLogicalAggregationStage` (Logical Stage)
