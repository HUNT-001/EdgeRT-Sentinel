"""edgert - EdgeRT Workbench CLI (Gate 1 commands)."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import typer

from . import doctor as doc
from . import scenario as sc
from .probe import parse_lines, probe_tcp

app = typer.Typer(add_completion=False, help="EdgeRT Workbench CLI")


@app.command()
def doctor(scope: str = typer.Option("all", help="qnx | hls | all")) -> None:
    """Check the toolchain for this machine. Exit 1 on any FAIL."""
    if scope not in ("qnx", "hls", "all"):
        raise typer.BadParameter("scope must be qnx, hls or all")
    checks = doc.run(scope)
    width = max(len(c.name) for c in checks)
    for c in checks:
        typer.echo(f"[{c.status}] {c.name:<{width}}  {c.detail}")
    failed = [c for c in checks if c.status == doc.FAIL]
    typer.echo(f"\n{len(checks) - len(failed)}/{len(checks)} checks not failing")
    raise typer.Exit(1 if failed else 0)


@app.command()
def probe(
    host: str = typer.Option(None, help="Target IP (TCP mode)"),
    port: int = typer.Option(5555),
    log: Path = typer.Option(None, help="Parse a captured serial/stdout log instead of TCP"),
    expect: int = typer.Option(200, help="Lines the target sends"),
    hz: float = typer.Option(20.0, help="Rate the target sends at"),
) -> None:
    """Validate link_probe output over TCP or from a serial log. Exit 1 on FAIL."""
    if (host is None) == (log is None):
        raise typer.BadParameter("give exactly one of --host or --log")
    try:
        rep = parse_lines(log.read_text(encoding="utf-8", errors="replace").splitlines()) if log \
            else probe_tcp(host, port)
    except ConnectionError as e:
        typer.echo(f"error: {e}", err=True)
        raise typer.Exit(2)
    s = rep.summary(expect, hz)
    typer.echo(json.dumps(s))
    typer.echo(f"GATE1 LINK PROBE: {'PASS' if s['pass'] else 'FAIL'}")
    raise typer.Exit(0 if s["pass"] else 1)


scenario_app = typer.Typer(help="Scenario files: validate, expand, summarize")
app.add_typer(scenario_app, name="scenario")


def _load(path: Path) -> dict:
    try:
        return sc.load(path)
    except (sc.ScenarioError, OSError) as e:
        typer.echo(f"error: {path}: {e}", err=True)
        raise typer.Exit(2)


@scenario_app.command("validate")
def scenario_validate(paths: list[Path]) -> None:
    """Validate scenario files. Exit 2 on the first invalid one."""
    for p in paths:
        _load(p)
        typer.echo(f"ok  {p}")


@scenario_app.command("summary")
def scenario_summary(path: Path) -> None:
    """Print expected-risk summary for a scenario (from the golden model)."""
    typer.echo(json.dumps(sc.summarize(sc.expand(_load(path))), indent=2))


@scenario_app.command("expand")
def scenario_expand(
    path: Path,
    out: Path = typer.Option(None, help="Write here (default stdout)"),
    fmt: str = typer.Option("jsonl", "--format", help="jsonl | csv (csv is what the target-side ipc_test reads)"),
) -> None:
    """Expand a scenario: one frame with inputs and expected output per line."""
    if fmt not in ("jsonl", "csv"):
        raise typer.BadParameter("format must be jsonl or csv")
    frames = sc.expand(_load(path))
    lines = sc.to_csv_lines(frames) if fmt == "csv" else [json.dumps(f, separators=(",", ":")) for f in frames]
    if out:
        out.write_text("\n".join(lines) + "\n", encoding="utf-8")
        typer.echo(f"wrote {len(lines)} frames to {out}")
    else:
        typer.echo("\n".join(lines))


if __name__ == "__main__":
    sys.exit(app())
