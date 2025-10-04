from opentelemetry import trace
from opentelemetry.trace.span import Span
from opentelemetry.trace.status import StatusCode


def get_default_current_span(attributes: dict = None) -> Span:
    current_span = trace.get_current_span()
    current_span.set_status(StatusCode.OK)
    if attributes:
        current_span.set_attributes(attributes)
    return current_span
