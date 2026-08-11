import base64
import json
import os
from logging import getLogger
from os import linesep
from typing import Callable, Optional, Sequence

from google.protobuf.json_format import MessageToDict
from opentelemetry.exporter.otlp.proto.common.trace_encoder import encode_spans
from opentelemetry.sdk.trace import ReadableSpan
from opentelemetry.sdk.trace.export import SpanExporter, SpanExportResult

logger = getLogger(__name__)


class FileSpanExporter(SpanExporter):
    """
    FileSpanExporter is an implementation of :class:`SpanExporter` that sends spans to files in directory.

    These files are in JSON format by default.
    """

    def __init__(
        self,
        directory: str,
        pretty_print=False,
        service_name: Optional[str] = None,
        formatter: Callable[[ReadableSpan], str] = lambda span: span.to_json() + linesep,
    ):
        self.formatter = formatter
        self.service_name = service_name
        self.file_count = 0
        self.pretty_print = pretty_print
        self.directory = directory

    def convert_span(self, span: dict) -> None:
        for key in ["traceId", "spanId", "parentSpanId"]:
            if key not in span:
                continue

            span[key] = base64.b64decode(span[key]).hex()

    def export(self, spans: Sequence[ReadableSpan]) -> SpanExportResult:
        self.file_count += 1
        file_name = f"metrics{self.file_count}.json"
        try:
            encoded_spans = encode_spans(spans)
            message = MessageToDict(encoded_spans)
            # Evergreen expects the ids in hex but the python otel library exportes them in base64
            for resourceSpan in message["resourceSpans"]:
                for scopeSpan in resourceSpan["scopeSpans"]:
                    for span in scopeSpan["spans"]:
                        self.convert_span(span)

            with open(os.path.join(self.directory, file_name), "w") as file:
                if self.pretty_print:
                    json.dump(message, file, indent=2)
                else:
                    json.dump(message, file, indent=None, separators=(",", ":"))
        except:
            logger.exception("Failed to write OTEL metrics to file %s", file_name)
            return SpanExportResult.FAILURE

        return SpanExportResult.SUCCESS

    def shutdown(self) -> None:
        pass

    def force_flush(self, timeout_millis: int = 30000) -> bool:
        return True
