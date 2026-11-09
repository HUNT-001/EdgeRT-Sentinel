import json
import socket
import threading

from edgert.probe import parse_lines, probe_tcp


def _line(seq: int, t_ms: int) -> str:
    return json.dumps({"probe": "link", "seq": seq, "t_ns": t_ms * 1_000_000})


def test_clean_stream_passes():
    rep = parse_lines(_line(i, i * 50) for i in range(10))
    s = rep.summary(expect=10, hz=20)
    assert s["pass"] and s["gaps"] == 0 and s["interarrival_p50_ms"] == 50.0


def test_gap_malformed_and_reorder_detected():
    lines = ["# /tmp/edgert/link_probe --stdout", _line(0, 0), _line(1, 50), _line(3, 150),
             '{"seq": broken', _line(2, 100), ""]
    rep = parse_lines(lines)
    assert (rep.received, rep.gaps, rep.malformed, rep.out_of_order) == (4, 1, 1, 1)
    assert not rep.summary(expect=4, hz=20)["pass"]


def test_wrong_probe_type_is_malformed():
    rep = parse_lines([json.dumps({"probe": "rt", "seq": 0, "t_ns": 0})])
    assert rep.malformed == 1 and rep.received == 0


def test_tcp_roundtrip():
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]

    def serve():
        c, _ = srv.accept()
        with c:
            c.sendall("".join(_line(i, i * 10) + "\n" for i in range(25)).encode())
        srv.close()

    threading.Thread(target=serve, daemon=True).start()
    rep = probe_tcp("127.0.0.1", port)
    assert rep.summary(expect=25, hz=100)["pass"]
