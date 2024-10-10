import json
from dataclasses import dataclass
from typing import Dict


@dataclass
class SpanMetrics:
    id: int
    name: str
    parent_id: int
    total_nanos: int
    net_nanos: int
    exclusive_nanos: int
    count: int
    children: Dict[str, int]

    def to_dict(self):
        return {
            "id": self.id,
            "name": self.name,
            "parentId": self.parent_id,
            "totalNanos": self.total_nanos,
            "netNanos": self.net_nanos,
            "exclusiveNanos": self.exclusive_nanos,
            "count": self.count,
        }


@dataclass
class CallMetrics:
    spans: Dict[int, SpanMetrics]

    @staticmethod
    def new_empty():
        spans = {0: SpanMetrics(0, "", 0, 0, 0, 0, 0, {})}
        return CallMetrics(spans)

    @staticmethod
    def from_json(json):
        spans = {0: SpanMetrics(0, "", 0, 0, 0, 0, 0, {})}
        for spanJson in json["spans"]:
            spans[int(spanJson["id"])] = SpanMetrics(
                int(spanJson["id"]),
                str(spanJson["name"]),
                int(spanJson["parentId"]),
                int(spanJson["totalNanos"]),
                int(spanJson["netNanos"]),
                int(spanJson["exclusiveNanos"]),
                int(spanJson["count"]),
                {},
            )

        for span in spans.values():
            if span.id == 0:
                continue
            spans[span.parent_id].children[span.name] = span.id

        return CallMetrics(spans)

    def add(self, other):
        self.visit_add(other, 0, 0, 1)

    def subtract(self, other):
        self.visit_add(other, 0, 0, -1)

    def add_weighted(self, other, weight):
        self.visit_add(other, 0, 0, weight)

    def visit_add(self, other, self_id: int, other_id: int, mult: float):
        self.spans[self_id].total_nanos += int(other.spans[other_id].total_nanos * mult)
        self.spans[self_id].net_nanos += int(other.spans[other_id].net_nanos * mult)
        self.spans[self_id].exclusive_nanos += int(other.spans[other_id].exclusive_nanos * mult)
        self.spans[self_id].count += int(other.spans[other_id].count * mult)
        for name, other_child_id in other.spans[other_id].children.items():
            self.visit_add(other, self.get_or_add_child_span(self_id, name), other_child_id, mult)

    def get_or_add_child_span(self, parent_id: int, name: str):
        if name in self.spans[parent_id].children:
            return self.spans[parent_id].children[name]

        span = SpanMetrics(len(self.spans), name, parent_id, 0, 0, 0, 0, {})
        self.spans[span.id] = span
        self.spans[parent_id].children[name] = span.id
        return span.id

    def find_span(self, path: str):
        span_id = 0
        for p in path.split("."):
            if span_id is None:
                return None

            span_id = self.spans[span_id].children[p]
        return span_id

    def to_dict(self):
        return {
            "spans": [
                s.to_dict() for s in sorted(self.spans.values(), key=lambda x: x.id) if s.id != 0
            ]
        }

    def to_json(self):
        return json.dumps(self.to_dict())
