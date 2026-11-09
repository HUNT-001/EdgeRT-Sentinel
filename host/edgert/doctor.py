"""Environment checks for Gate 1. Pure stdlib so it runs before deps exist."""

from __future__ import annotations

import os
import platform
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path

PASS, WARN, FAIL = "PASS", "WARN", "FAIL"


@dataclass
class Check:
    name: str
    status: str
    detail: str


def _ram_gb() -> float | None:
    try:
        if sys.platform.startswith("linux"):
            for line in Path("/proc/meminfo").read_text().splitlines():
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) / 1024**2
        if sys.platform == "win32":
            import ctypes

            class MEMSTAT(ctypes.Structure):
                _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                            ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                            ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                            ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                            ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

            m = MEMSTAT()
            m.dwLength = ctypes.sizeof(MEMSTAT)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
            return m.ullTotalPhys / 1024**3
    except Exception:
        return None
    return None


def _tool(name: str, required: bool, hint: str) -> Check:
    path = shutil.which(name)
    if path:
        return Check(name, PASS, path)
    return Check(name, FAIL if required else WARN, f"not on PATH - {hint}")


def qnx_checks() -> list[Check]:
    """Ubuntu VM: QNX SDP, QEMU, SSH."""
    out: list[Check] = []
    osr = Path("/etc/os-release")
    distro = ""
    if osr.exists():
        info = dict(l.split("=", 1) for l in osr.read_text().splitlines() if "=" in l)
        distro = f"{info.get('NAME', '').strip(chr(34))} {info.get('VERSION_ID', '').strip(chr(34))}"
    ok = "Ubuntu" in distro and any(v in distro for v in ("22.04", "24.04"))
    out.append(Check("os", PASS if ok else WARN, distro or platform.platform()))
    out.append(Check("kvm", PASS if Path("/dev/kvm").exists() else WARN,
                     "/dev/kvm present" if Path("/dev/kvm").exists()
                     else "no /dev/kvm - enable nested virtualisation or QEMU will be very slow"))
    for var in ("QNX_HOST", "QNX_TARGET"):
        val = os.environ.get(var)
        out.append(Check(var, PASS if val and Path(val).is_dir() else FAIL,
                         val or "unset - source ~/qnx800/qnxsdp-env.sh"))
    out.append(_tool("q++", True, "source the SDP env script"))
    out.append(_tool("mkqnximage", True, "install SDP 8.0 via QNX Software Center"))
    out.append(_tool("qemu-system-x86_64", True, "sudo apt install qemu-system-x86"))
    out.append(_tool("ssh", True, "sudo apt install openssh-client"))
    return out


def hls_checks() -> list[Check]:
    """Wherever Vitis HLS runs (Windows host or Linux)."""
    out = [_tool("vitis-run", True, "source/run settings64 from the Vitis 2026.1 install"),
           _tool("v++", True, "source/run settings64 from the Vitis 2026.1 install")]
    ram = _ram_gb()
    if ram is not None:
        out.append(Check("ram", PASS if ram >= 30 else WARN,
                         f"{ram:.1f} GB (AMD lists 32 GB minimum; do not run Vitis inside a VM with less)"))
    return out


def common_checks() -> list[Check]:
    v = sys.version_info
    return [Check("python", PASS if v >= (3, 10) else FAIL, platform.python_version())]


def run(scope: str) -> list[Check]:
    checks = common_checks()
    if scope in ("qnx", "all"):
        checks += qnx_checks()
    if scope in ("hls", "all"):
        checks += hls_checks()
    return checks
