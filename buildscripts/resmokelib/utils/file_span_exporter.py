from typing import Optional, Callable, Sequence
from os import linesep
from logging import getLogger

from opentelemetry.sdk.trace.export import SpanExporter, SpanExportResult
from opentelemetry.sdk.trace import ReadableSpan

logger = getLogger(__name__)


class FileSpanExporter(SpanExporter):
    """
    FileSpanExporter is an implementation of :class:`SpanExporter` that sends spans to a file.

    This class is responsible for a file handle.
    It correctly closes the file handle on shutdown.
    """

    def __init__(
            self,
            file_name: str,
            service_name: Optional[str] = None,
            formatter: Callable[[ReadableSpan], str] = lambda span: span.to_json() + linesep,
    ):
        self.formatter = formatter
        self.service_name = service_name
        self.out = open(file_name, mode='w')

    def export(self, spans: Sequence[ReadableSpan]) -> SpanExportResult:
        try:
            for span in spans:
                self.out.write(self.formatter(span))
            self.out.flush()
        except:
            logger.exception("Failed to write OTEL metrics to file %s", self.out.name)
            return SpanExportResult.FAILURE

        return SpanExportResult.SUCCESS

    def shutdown(self) -> None:
        self.out.close()

    def force_flush(self, timeout_millis: int = 30000) -> bool:
        return True
