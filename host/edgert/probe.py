"""Host side of link_probe: read JSON lines from the target and validate them."""

from __future__ import annotations

import json
import socket
import statistics
import time
from dataclasses import dataclass, field
from typing import Iterable, TextIO


@dataclass
class LinkReport:
    received: int = 0
    malformed: int = 0
    gaps: int = 0               # missing sequence numbers
    out_of_order: int = 0
    interarrival_ms: list[float] = field(default_factory=list)  # target-clock deltas

    def summary(self, expect: int, hz: float) -> dict:
        ia = self.interarrival_ms
        nominal = 1000.0 / hz
        return {
            "expected": expect,
            "received": self.received,
            "malformed": self.malformed,
            "gaps": self.gaps,
            "out_of_order": self.out_of_order,
            "nominal_ms": round(nominal, 3),
            "interarrival_p50_ms": round(statistics.median(ia), 3) if ia else None,
            "interarrival_max_ms": round(max(ia), 3) if ia else None,
            "pass": self.received == expect and self.malformed == 0 and self.gaps == 0 and self.out_of_order == 0,
        }


def parse_lines(lines: Iterable[str]) -> LinkReport:
    rep = LinkReport()
    last_seq: int | None = None
    last_t: int | None = None
    for raw in lines:
        raw = raw.strip()
        if not raw.startswith("{"):
            continue  # blank lines and serial console noise (prompts, echoes)
        try:
            msg = json.loads(raw)
            seq, t_ns = int(msg["seq"]), int(msg["t_ns"])
            if msg.get("probe") != "link":
                raise ValueError("not a link line")
        except (ValueError, KeyError, TypeError):
            rep.malformed += 1
            continue
        rep.received += 1
        if last_seq is not None:
            if seq <= last_seq:
                rep.out_of_order += 1
                continue
            rep.gaps += seq - last_seq - 1
            if last_t is not None:
                rep.interarrival_ms.append((t_ns - last_t) / 1e6)
        last_seq, last_t = seq, t_ns
    return rep


def _socket_lines(sock: socket.socket) -> Iterable[str]:
    f: TextIO = sock.makefile("r", encoding="utf-8", newline="\n")
    yield from f


def probe_tcp(host: str, port: int, timeout_s: float = 30.0, retries: int = 10) -> LinkReport:
    """Connect (retrying while the target starts listening) and read until EOF."""
    last_err: Exception | None = None
    for _ in range(retries):
        try:
            with socket.create_connection((host, port), timeout=5) as s:
                s.settimeout(timeout_s)
                return parse_lines(_socket_lines(s))
        except (ConnectionRefusedError, socket.timeout, OSError) as e:
            last_err = e
            time.sleep(0.5)
    raise ConnectionError(f"could not reach {host}:{port}: {last_err}")
