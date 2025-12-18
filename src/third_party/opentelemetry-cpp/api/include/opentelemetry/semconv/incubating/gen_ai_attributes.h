/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_attributes-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace gen_ai
{

/**
  Free-form description of the GenAI agent provided by the application.
 */
static constexpr const char *kGenAiAgentDescription = "gen_ai.agent.description";

/**
  The unique identifier of the GenAI agent.
 */
static constexpr const char *kGenAiAgentId = "gen_ai.agent.id";

/**
  Human-readable name of the GenAI agent provided by the application.
 */
static constexpr const char *kGenAiAgentName = "gen_ai.agent.name";

/**
  Deprecated, use Event API to report completions contents.

  @deprecated
  {"note": "Removed, no replacement at this time.", "reason": "obsoleted"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kGenAiCompletion = "gen_ai.completion";

/**
  The unique identifier for a conversation (session, thread), used to store and correlate messages
  within this conversation.
 */
static constexpr const char *kGenAiConversationId = "gen_ai.conversation.id";

/**
  The data source identifier.
  <p>
  Data sources are used by AI agents and RAG applications to store grounding data. A data source may
  be an external database, object store, document collection, website, or any other storage system
  used by the GenAI agent or application. The @code gen_ai.data_source.id @endcode SHOULD match the
  identifier used by the GenAI system rather than a name specific to the external storage, such as a
  database or object store. Semantic conventions referencing @code gen_ai.data_source.id @endcode
  MAY also leverage additional attributes, such as @code db.* @endcode, to further identify and
  describe the data source.
 */
static constexpr const char *kGenAiDataSourceId = "gen_ai.data_source.id";

/**
  The number of dimensions the resulting output embeddings should have.
 */
static constexpr const char *kGenAiEmbeddingsDimensionCount = "gen_ai.embeddings.dimension.count";

/**
  A free-form explanation for the assigned score provided by the evaluator.
 */
static constexpr const char *kGenAiEvaluationExplanation = "gen_ai.evaluation.explanation";

/**
  The name of the evaluation metric used for the GenAI response.
 */
static constexpr const char *kGenAiEvaluationName = "gen_ai.evaluation.name";

/**
  Human readable label for evaluation.
  <p>
  This attribute provides a human-readable interpretation of the evaluation score produced by an
  evaluator. For example, a score value of 1 could mean "relevant" in one evaluation system and "not
  relevant" in another, depending on the scoring range and evaluator. The label SHOULD have low
  cardinality. Possible values depend on the evaluation metric and evaluator used; implementations
  SHOULD document the possible values.
 */
static constexpr const char *kGenAiEvaluationScoreLabel = "gen_ai.evaluation.score.label";

/**
  The evaluation score returned by the evaluator.
 */
static constexpr const char *kGenAiEvaluationScoreValue = "gen_ai.evaluation.score.value";

/**
  The chat history provided to the model as an input.
  <p>
  Instrumentations MUST follow <a href="/docs/gen-ai/gen-ai-input-messages.json">Input messages JSON
  schema</a>. When the attribute is recorded on events, it MUST be recorded in structured form. When
  recorded on spans, it MAY be recorded as a JSON string if structured format is not supported and
  SHOULD be recorded in structured form otherwise. <p> Messages MUST be provided in the order they
  were sent to the model. Instrumentations MAY provide a way for users to filter or truncate input
  messages. <blockquote>
  [!Warning]
  This attribute is likely to contain sensitive information including user/PII data.</blockquote>
  <p>
  See <a href="/docs/gen-ai/gen-ai-spans.md#recording-content-on-attributes">Recording content on
  attributes</a> section for more details.
 */
static constexpr const char *kGenAiInputMessages = "gen_ai.input.messages";

/**
  Deprecated, use @code gen_ai.output.type @endcode.

  @deprecated
  {"note": "Replaced by @code gen_ai.output.type @endcode.", "reason": "renamed", "renamed_to":
  "gen_ai.output.type"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kGenAiOpenaiRequestResponseFormat =
    "gen_ai.openai.request.response_format";

/**
  Deprecated, use @code gen_ai.request.seed @endcode.

  @deprecated
  {"note": "Replaced by @code gen_ai.request.seed @endcode.", "reason": "renamed", "renamed_to":
  "gen_ai.request.seed"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kGenAiOpenaiRequestSeed =
    "gen_ai.openai.request.seed";

/**
  Deprecated, use @code openai.request.service_tier @endcode.

  @deprecated
  {"note": "Replaced by @code openai.request.service_tier @endcode.", "reason": "renamed",
  "renamed_to": "openai.request.service_tier"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kGenAiOpenaiRequestServiceTier =
    "gen_ai.openai.request.service_tier";

/**
  Deprecated, use @code openai.response.service_tier @endcode.

  @deprecated
  {"note": "Replaced by @code openai.response.service_tier @endcode.", "reason": "renamed",
  "renamed_to": "openai.response.service_tier"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kGenAiOpenaiResponseServiceTier =
    "gen_ai.openai.response.service_tier";

/**
  Deprecated, use @code openai.response.system_fingerprint @endcode.

  @deprecated
  {"note": "Replaced by @code openai.response.system_fingerprint @endcode.", "reason": "renamed",
  "renamed_to": "openai.response.system_fingerprint"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kGenAiOpenaiResponseSystemFingerprint =
    "gen_ai.openai.response.system_fingerprint";

/**
  The name of the operation being performed.
  <p>
  If one of the predefined values applies, but specific system uses a different name it's
  RECOMMENDED to document it in the semantic conventions for specific GenAI system and use
  system-specific name in the instrumentation. If a different name is not documented,
  instrumentation libraries SHOULD use applicable predefined value.
 */
static constexpr const char *kGenAiOperationName = "gen_ai.operation.name";

/**
  Messages returned by the model where each message represents a specific model response (choice,
  candidate). <p> Instrumentations MUST follow <a
  href="/docs/gen-ai/gen-ai-output-messages.json">Output messages JSON schema</a> <p> Each message
  represents a single output choice/candidate generated by the model. Each message corresponds to
  exactly one generation (choice/candidate) and vice versa - one choice cannot be split across
  multiple messages or one message cannot contain parts from multiple choices.
  <p>
  When the attribute is recorded on events, it MUST be recorded in structured
  form. When recorded on spans, it MAY be recorded as a JSON string if structured
  format is not supported and SHOULD be recorded in structured form otherwise.
  <p>
  Instrumentations MAY provide a way for users to filter or truncate
  output messages.
  <blockquote>
  [!Warning]
  This attribute is likely to contain sensitive information including user/PII data.</blockquote>
  <p>
  See <a href="/docs/gen-ai/gen-ai-spans.md#recording-content-on-attributes">Recording content on
  attributes</a> section for more details.
 */
static constexpr const char *kGenAiOutputMessages = "gen_ai.output.messages";

/**
  Represents the content type requested by the client.
  <p>
  This attribute SHOULD be used when the client requests output of a specific type. The model may
  return zero or more outputs of this type. This attribute specifies the output modality and not the
  actual output format. For example, if an image is requested, the actual output could be a URL
  pointing to an image file. Additional output format details may be recorded in the future in the
  @code gen_ai.output.{type}.* @endcode attributes.
 */
static constexpr const char *kGenAiOutputType = "gen_ai.output.type";

/**
  Deprecated, use Event API to report prompt contents.

  @deprecated
  {"note": "Removed, no replacement at this time.", "reason": "obsoleted"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kGenAiPrompt = "gen_ai.prompt";

/**
  The Generative AI provider as identified by the client or server instrumentation.
  <p>
  The attribute SHOULD be set based on the instrumentation's best
  knowledge and may differ from the actual model provider.
  <p>
  Multiple providers, including Azure OpenAI, Gemini, and AI hosting platforms
  are accessible using the OpenAI REST API and corresponding client libraries,
  but may proxy or host models from different providers.
  <p>
  The @code gen_ai.request.model @endcode, @code gen_ai.response.model @endcode, and @code
  server.address @endcode attributes may help identify the actual system in use. <p> The @code
  gen_ai.provider.name @endcode attribute acts as a discriminator that identifies the GenAI
  telemetry format flavor specific to that provider within GenAI semantic conventions. It SHOULD be
  set consistently with provider-specific attributes and signals. For example, GenAI spans, metrics,
  and events related to AWS Bedrock should have the @code gen_ai.provider.name @endcode set to @code
  aws.bedrock @endcode and include applicable @code aws.bedrock.* @endcode attributes and are not
  expected to include
  @code openai.* @endcode attributes.
 */
static constexpr const char *kGenAiProviderName = "gen_ai.provider.name";

/**
  The target number of candidate completions to return.
 */
static constexpr const char *kGenAiRequestChoiceCount = "gen_ai.request.choice.count";

/**
  The encoding formats requested in an embeddings operation, if specified.
  <p>
  In some GenAI systems the encoding formats are called embedding types. Also, some GenAI systems
  only accept a single format per request.
 */
static constexpr const char *kGenAiRequestEncodingFormats = "gen_ai.request.encoding_formats";

/**
  The frequency penalty setting for the GenAI request.
 */
static constexpr const char *kGenAiRequestFrequencyPenalty = "gen_ai.request.frequency_penalty";

/**
  The maximum number of tokens the model generates for a request.
 */
static constexpr const char *kGenAiRequestMaxTokens = "gen_ai.request.max_tokens";

/**
  The name of the GenAI model a request is being made to.
 */
static constexpr const char *kGenAiRequestModel = "gen_ai.request.model";

/**
  The presence penalty setting for the GenAI request.
 */
static constexpr const char *kGenAiRequestPresencePenalty = "gen_ai.request.presence_penalty";

/**
  Requests with same seed value more likely to return same result.
 */
static constexpr const char *kGenAiRequestSeed = "gen_ai.request.seed";

/**
  List of sequences that the model will use to stop generating further tokens.
 */
static constexpr const char *kGenAiRequestStopSequences = "gen_ai.request.stop_sequences";

/**
  The temperature setting for the GenAI request.
 */
static constexpr const char *kGenAiRequestTemperature = "gen_ai.request.temperature";

/**
  The top_k sampling setting for the GenAI request.
 */
static constexpr const char *kGenAiRequestTopK = "gen_ai.request.top_k";

/**
  The top_p sampling setting for the GenAI request.
 */
static constexpr const char *kGenAiRequestTopP = "gen_ai.request.top_p";

/**
  Array of reasons the model stopped generating tokens, corresponding to each generation received.
 */
static constexpr const char *kGenAiResponseFinishReasons = "gen_ai.response.finish_reasons";

/**
  The unique identifier for the completion.
 */
static constexpr const char *kGenAiResponseId = "gen_ai.response.id";

/**
  The name of the model that generated the response.
 */
static constexpr const char *kGenAiResponseModel = "gen_ai.response.model";

/**
  Deprecated, use @code gen_ai.provider.name @endcode instead.

  @deprecated
  {"note": "Replaced by @code gen_ai.provider.name @endcode.", "reason": "renamed", "renamed_to":
  "gen_ai.provider.name"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kGenAiSystem = "gen_ai.system";

/**
  The system message or instructions provided to the GenAI model separately from the chat history.
  <p>
  This attribute SHOULD be used when the corresponding provider or API
  allows to provide system instructions or messages separately from the
  chat history.
  <p>
  Instructions that are part of the chat history SHOULD be recorded in
  @code gen_ai.input.messages @endcode attribute instead.
  <p>
  Instrumentations MUST follow <a href="/docs/gen-ai/gen-ai-system-instructions.json">System
  instructions JSON schema</a>. <p> When recorded on spans, it MAY be recorded as a JSON string if
  structured format is not supported and SHOULD be recorded in structured form otherwise. <p>
  Instrumentations MAY provide a way for users to filter or truncate
  system instructions.
  <blockquote>
  [!Warning]
  This attribute may contain sensitive information.</blockquote>
  <p>
  See <a href="/docs/gen-ai/gen-ai-spans.md#recording-content-on-attributes">Recording content on
  attributes</a> section for more details.
 */
static constexpr const char *kGenAiSystemInstructions = "gen_ai.system_instructions";

/**
  The type of token being counted.
 */
static constexpr const char *kGenAiTokenType = "gen_ai.token.type";

/**
  Parameters passed to the tool call.
  <blockquote>
  [!WARNING]
  This attribute may contain sensitive information.</blockquote>
  <p>
  It's expected to be an object - in case a serialized string is available
  to the instrumentation, the instrumentation SHOULD do the best effort to
  deserialize it to an object. When recorded on spans, it MAY be recorded as a JSON string if
  structured format is not supported and SHOULD be recorded in structured form otherwise.
 */
static constexpr const char *kGenAiToolCallArguments = "gen_ai.tool.call.arguments";

/**
  The tool call identifier.
 */
static constexpr const char *kGenAiToolCallId = "gen_ai.tool.call.id";

/**
  The result returned by the tool call (if any and if execution was successful).
  <blockquote>
  [!WARNING]
  This attribute may contain sensitive information.</blockquote>
  <p>
  It's expected to be an object - in case a serialized string is available
  to the instrumentation, the instrumentation SHOULD do the best effort to
  deserialize it to an object. When recorded on spans, it MAY be recorded as a JSON string if
  structured format is not supported and SHOULD be recorded in structured form otherwise.
 */
static constexpr const char *kGenAiToolCallResult = "gen_ai.tool.call.result";

/**
  The list of source system tool definitions available to the GenAI agent or model.
  <p>
  The value of this attribute matches source system tool definition format.
  <p>
  It's expected to be an array of objects where each object represents a tool definition. In case a
  serialized string is available to the instrumentation, the instrumentation SHOULD do the best
  effort to deserialize it to an array. When recorded on spans, it MAY be recorded as a JSON string
  if structured format is not supported and SHOULD be recorded in structured form otherwise. <p>
  Since this attribute could be large, it's NOT RECOMMENDED to populate
  it by default. Instrumentations MAY provide a way to enable
  populating this attribute.
 */
static constexpr const char *kGenAiToolDefinitions = "gen_ai.tool.definitions";

/**
  The tool description.
 */
static constexpr const char *kGenAiToolDescription = "gen_ai.tool.description";

/**
  Name of the tool utilized by the agent.
 */
static constexpr const char *kGenAiToolName = "gen_ai.tool.name";

/**
  Type of the tool utilized by the agent
  <p>
  Extension: A tool executed on the agent-side to directly call external APIs, bridging the gap
  between the agent and real-world systems. Agent-side operations involve actions that are performed
  by the agent on the server or within the agent's controlled environment. Function: A tool executed
  on the client-side, where the agent generates parameters for a predefined function, and the client
  executes the logic. Client-side operations are actions taken on the user's end or within the
  client application. Datastore: A tool used by the agent to access and query structured or
  unstructured external data for retrieval-augmented tasks or knowledge updates.
 */
static constexpr const char *kGenAiToolType = "gen_ai.tool.type";

/**
  Deprecated, use @code gen_ai.usage.output_tokens @endcode instead.

  @deprecated
  {"note": "Replaced by @code gen_ai.usage.output_tokens @endcode.", "reason": "renamed",
  "renamed_to": "gen_ai.usage.output_tokens"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kGenAiUsageCompletionTokens =
    "gen_ai.usage.completion_tokens";

/**
  The number of tokens used in the GenAI input (prompt).
 */
static constexpr const char *kGenAiUsageInputTokens = "gen_ai.usage.input_tokens";

/**
  The number of tokens used in the GenAI response (completion).
 */
static constexpr const char *kGenAiUsageOutputTokens = "gen_ai.usage.output_tokens";

/**
  Deprecated, use @code gen_ai.usage.input_tokens @endcode instead.

  @deprecated
  {"note": "Replaced by @code gen_ai.usage.input_tokens @endcode.", "reason": "renamed",
  "renamed_to": "gen_ai.usage.input_tokens"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kGenAiUsagePromptTokens =
    "gen_ai.usage.prompt_tokens";

namespace GenAiOpenaiRequestResponseFormatValues
{
/**
  Text response format
 */
static constexpr const char *kText = "text";

/**
  JSON object response format
 */
static constexpr const char *kJsonObject = "json_object";

/**
  JSON schema response format
 */
static constexpr const char *kJsonSchema = "json_schema";

}  // namespace GenAiOpenaiRequestResponseFormatValues

namespace GenAiOpenaiRequestServiceTierValues
{
/**
  The system will utilize scale tier credits until they are exhausted.
 */
static constexpr const char *kAuto = "auto";

/**
  The system will utilize the default scale tier.
 */
static constexpr const char *kDefault = "default";

}  // namespace GenAiOpenaiRequestServiceTierValues

namespace GenAiOperationNameValues
{
/**
  Chat completion operation such as <a
  href="https://platform.openai.com/docs/api-reference/chat">OpenAI Chat API</a>
 */
static constexpr const char *kChat = "chat";

/**
  Multimodal content generation operation such as <a
  href="https://ai.google.dev/api/generate-content">Gemini Generate Content</a>
 */
static constexpr const char *kGenerateContent = "generate_content";

/**
  Text completions operation such as <a
  href="https://platform.openai.com/docs/api-reference/completions">OpenAI Completions API
  (Legacy)</a>
 */
static constexpr const char *kTextCompletion = "text_completion";

/**
  Embeddings operation such as <a
  href="https://platform.openai.com/docs/api-reference/embeddings/create">OpenAI Create embeddings
  API</a>
 */
static constexpr const char *kEmbeddings = "embeddings";

/**
  Create GenAI agent
 */
static constexpr const char *kCreateAgent = "create_agent";

/**
  Invoke GenAI agent
 */
static constexpr const char *kInvokeAgent = "invoke_agent";

/**
  Execute a tool
 */
static constexpr const char *kExecuteTool = "execute_tool";

}  // namespace GenAiOperationNameValues

namespace GenAiOutputTypeValues
{
/**
  Plain text
 */
static constexpr const char *kText = "text";

/**
  JSON object with known or unknown schema
 */
static constexpr const char *kJson = "json";

/**
  Image
 */
static constexpr const char *kImage = "image";

/**
  Speech
 */
static constexpr const char *kSpeech = "speech";

}  // namespace GenAiOutputTypeValues

namespace GenAiProviderNameValues
{
/**
  <a href="https://openai.com/">OpenAI</a>
 */
static constexpr const char *kOpenai = "openai";

/**
  Any Google generative AI endpoint
 */
static constexpr const char *kGcpGenAi = "gcp.gen_ai";

/**
  <a href="https://cloud.google.com/vertex-ai">Vertex AI</a>
 */
static constexpr const char *kGcpVertexAi = "gcp.vertex_ai";

/**
  <a href="https://cloud.google.com/products/gemini">Gemini</a>
 */
static constexpr const char *kGcpGemini = "gcp.gemini";

/**
  <a href="https://www.anthropic.com/">Anthropic</a>
 */
static constexpr const char *kAnthropic = "anthropic";

/**
  <a href="https://cohere.com/">Cohere</a>
 */
static constexpr const char *kCohere = "cohere";

/**
  Azure AI Inference
 */
static constexpr const char *kAzureAiInference = "azure.ai.inference";

/**
  <a href="https://azure.microsoft.com/products/ai-services/openai-service/">Azure OpenAI</a>
 */
static constexpr const char *kAzureAiOpenai = "azure.ai.openai";

/**
  <a href="https://www.ibm.com/products/watsonx-ai">IBM Watsonx AI</a>
 */
static constexpr const char *kIbmWatsonxAi = "ibm.watsonx.ai";

/**
  <a href="https://aws.amazon.com/bedrock">AWS Bedrock</a>
 */
static constexpr const char *kAwsBedrock = "aws.bedrock";

/**
  <a href="https://www.perplexity.ai/">Perplexity</a>
 */
static constexpr const char *kPerplexity = "perplexity";

/**
  <a href="https://x.ai/">xAI</a>
 */
static constexpr const char *kXAi = "x_ai";

/**
  <a href="https://www.deepseek.com/">DeepSeek</a>
 */
static constexpr const char *kDeepseek = "deepseek";

/**
  <a href="https://groq.com/">Groq</a>
 */
static constexpr const char *kGroq = "groq";

/**
  <a href="https://mistral.ai/">Mistral AI</a>
 */
static constexpr const char *kMistralAi = "mistral_ai";

}  // namespace GenAiProviderNameValues

namespace GenAiSystemValues
{
/**
  OpenAI
 */
static constexpr const char *kOpenai = "openai";

/**
  Any Google generative AI endpoint
 */
static constexpr const char *kGcpGenAi = "gcp.gen_ai";

/**
  Vertex AI
 */
static constexpr const char *kGcpVertexAi = "gcp.vertex_ai";

/**
  Gemini
 */
static constexpr const char *kGcpGemini = "gcp.gemini";

/**
  Vertex AI

  @deprecated
  {"note": "Replaced by @code gcp.vertex_ai @endcode.", "reason": "renamed", "renamed_to":
  "gcp.vertex_ai"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kVertexAi = "vertex_ai";

/**
  Gemini

  @deprecated
  {"note": "Replaced by @code gcp.gemini @endcode.", "reason": "renamed", "renamed_to":
  "gcp.gemini"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kGemini = "gemini";

/**
  Anthropic
 */
static constexpr const char *kAnthropic = "anthropic";

/**
  Cohere
 */
static constexpr const char *kCohere = "cohere";

/**
  Azure AI Inference

  @deprecated
  {"note": "Replaced by @code azure.ai.inference @endcode.", "reason": "renamed", "renamed_to":
  "azure.ai.inference"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kAzAiInference = "az.ai.inference";

/**
  Azure OpenAI

  @deprecated
  {"note": "Replaced by @code azure.ai.openai @endcode.", "reason": "renamed", "renamed_to":
  "azure.ai.openai"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kAzAiOpenai = "az.ai.openai";

/**
  Azure AI Inference
 */
static constexpr const char *kAzureAiInference = "azure.ai.inference";

/**
  Azure OpenAI
 */
static constexpr const char *kAzureAiOpenai = "azure.ai.openai";

/**
  IBM Watsonx AI
 */
static constexpr const char *kIbmWatsonxAi = "ibm.watsonx.ai";

/**
  AWS Bedrock
 */
static constexpr const char *kAwsBedrock = "aws.bedrock";

/**
  Perplexity
 */
static constexpr const char *kPerplexity = "perplexity";

/**
  xAI
 */
static constexpr const char *kXai = "xai";

/**
  DeepSeek
 */
static constexpr const char *kDeepseek = "deepseek";

/**
  Groq
 */
static constexpr const char *kGroq = "groq";

/**
  Mistral AI
 */
static constexpr const char *kMistralAi = "mistral_ai";

}  // namespace GenAiSystemValues

namespace GenAiTokenTypeValues
{
/**
  Input tokens (prompt, input, etc.)
 */
static constexpr const char *kInput = "input";

/**
  Output tokens (completion, response, etc.)

  @deprecated
  {"note": "Replaced by @code output @endcode.", "reason": "renamed", "renamed_to": "output"}
 */
OPENTELEMETRY_DEPRECATED static constexpr const char *kCompletion = "output";

/**
  Output tokens (completion, response, etc.)
 */
static constexpr const char *kOutput = "output";

}  // namespace GenAiTokenTypeValues

}  // namespace gen_ai
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
