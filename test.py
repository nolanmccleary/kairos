#!/usr/bin/env python3
from __future__ import annotations

import argparse, csv, re, socket, struct, subprocess, sys, time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Sequence

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator


ROOT        = Path(__file__).resolve().parent
TEST_ELF    = ROOT / "build" / "test.elf"
OPENOCD_CFG = ROOT / "openocd" / "de1soc.cfg"
RESULTS_DIR = ROOT / "test_results"

EDF_SECONDS   = 12.0
EDF_TASKS     = 3
BAL_TASKS     = 6         # load-balance test task count (must match workload_edf.c BAL_NTASKS)
CTXSW_SAMPLES = 1024      # micro sample-ring sizes (must match workload_micro.c)
IRQ_SAMPLES   = 1024
MUTEX_SAMPLES = 1024
MEMLAT_SAMPLES = 8192     # matches workload_micro.c (bumped to catch SDRAM's spread)
ALLOC_METRICS = 7
TRACE_TICKS   = 7200      # g_sched_trace capacity (must match workload_edf.c)
GANTT_WINDOW  = 720       # ticks shown in the Gantt (>1 hyperperiod)
COMFORTABLE_U = 700       # always plot this under-capacity trial as a baseline
SUSP_TICKS    = 64        # suspend-test per-scenario run-tick capture depth (must match workload_suspend.c)
SUSP_SLACK    = 3         # ticks of scheduling slack allowed on a measured suspension delay

# persistent-thread test (must match workload_persistent.c)
PP_NPERIODIC  = 6         # tight periodic taskset, auto-balanced
PP_NPERSIST   = 5         # persistent threads: 3 HIGH + 1 MED + 1 LOW
PP_HI_IDS     = (10, 11, 12)   # same-priority level -> round-robin among these
PP_MED_ID     = 20             # below HIGH -> must starve while any HIGH is alive
PP_LOW_ID     = 30             # below MED  -> must starve
PP_SLICE      = 200       # PERSISTENT_SLICE_TICKS: a persistent thread's run-tick quantum
PP_SECONDS    = 8.0       # measurement window (> TRACE_TICKS ticks, so the trace fills)
PP_IDLE_ID    = 0xFF      # IDLE_ID: scheduler idle in the trace


# --- target ABI -------------------------------------------------------------
# g_metrics[]  metric_t : min, max, sum, count   (mean = sum/count)
# One name per slot, in slot order -- the host<->target ABI. Slots are written by
# workload_allocbench.c (0-3), workload_rmw.c (4-5), workload_matmul.c (6).
METRIC_STRUCT = struct.Struct("<IIII")
METRIC_SIZE   = METRIC_STRUCT.size
ALLOC_NAMES   = ["malloc", "free", "malloc_loaded", "free_loaded",
                 "mem_walk_8k", "mem_walk_256k", "matmul_32"]

# g_alloc_samples[ALLOC_OP_COUNT][ALLOC_ITERS] u32 : every per-iteration cycle
# count for the four allocator ops, for the timing-distribution plot. Dimensions
# must match ALLOC_OP_COUNT / ALLOC_ITERS in workload_allocbench.c.
ALLOC_SAMPLE_OPS   = ALLOC_NAMES[:4]
ALLOC_SAMPLE_ITERS = 512

# g_rmw_samples[2][RMW_ROW_LEN] u32 : per-pass cycle counts for the two RMW sweeps
# (row 0 = 8KB/64 passes, row 1 = 256KB/16 passes). Must match workload_rmw.c.
RMW_ROW_LEN    = 64
RMW_ROW_PASSES = [64, 16]
RMW_LABELS     = ["8 KB (fits L1)", "256 KB (fits L2)"]

# g_matmul_samples[MATMUL_REPS] u32 : per-rep cycle counts. Must match workload_matmul.c.
MATMUL_REPS = 8

# g_edf_metrics[NTASKS] metrics_t : ci, ci_av, prev_cycles, delta_sum, ti, ti_av, t0, prev_u
EDF_METRIC_STRUCT = struct.Struct("<8I")
EDF_METRIC_SIZE   = EDF_METRIC_STRUCT.size

# g_fault[NUM_CPUS] fault_record_t : magic, vec, pc, spsr, dfsr, dfar, ifsr, ifar
FAULT_STRUCT = struct.Struct("<8I")
FAULT_MAGIC  = 0x464C5431   # "FLT1"
FAULT_MSG_LEN = 128         # g_error_message[NUM_CPUS][FAULT_MSG_LEN] (must match fault.h)
FAULT_VEC    = {1: "undef", 2: "swi", 3: "prefetch abort", 4: "data abort", 5: "fiq"}
FAULT_FS     = {0b00001: "alignment", 0b00101: "translation (L1)", 0b00111: "translation (L2)",
                0b01000: "sync external abort", 0b01001: "domain (L1)", 0b01011: "domain (L2)",
                0b01101: "permission (L1)", 0b01111: "permission (L2)", 0b10110: "async external abort"}


def fault_report(words: Sequence[int], core: int | None = None) -> str | None:
    if len(words) < 8 or words[0] != FAULT_MAGIC:
        return None
    _, vec, pc, spsr, dfsr, dfar, ifsr, ifar = words[:8]
    fs = lambda s: FAULT_FS.get(((s >> 6) & 0x10) | (s & 0xF), f"FS=0b{((s>>6)&0x10)|(s&0xF):05b}")
    tag = f" (CPU{core})" if core is not None else ""
    out = [f"*** CPU FAULT{tag}: {FAULT_VEC.get(vec, f'vec={vec}')} ***",
           f"  faulting PC = 0x{pc:08x}",
           f"  SPSR        = 0x{spsr:08x}  (mode 0x{spsr & 0x1f:02x})"]
    if vec == 4:
        out += [f"  DFSR        = 0x{dfsr:08x}  ({fs(dfsr)}, {'write' if (dfsr>>11)&1 else 'read'})",
                f"  DFAR        = 0x{dfar:08x}"]
    elif vec == 3:
        out += [f"  IFSR        = 0x{ifsr:08x}  ({fs(ifsr)})", f"  IFAR        = 0x{ifar:08x}"]
    return "\n".join(out)


# --- result types -----------------------------------------------------------
@dataclass(frozen=True)
class Metric:
    name: str
    count: int
    total: int
    minimum: int
    maximum: int

    @property
    def mean(self) -> int:
        return self.total // self.count if self.count else 0


@dataclass(frozen=True)
class EDFResult:
    index: int
    u_permille: int
    ticks: int
    misses: int
    c: tuple[int, ...]
    periods: tuple[int, ...]
    done: tuple[int, ...]
    expected: tuple[int, ...]
    ci_av: tuple[int, ...]
    ti_av: tuple[int, ...]
    ci: tuple[int, ...]
    sched_overhead: tuple[int, ...]   # per-CPU EWMA scheduler cost, cycles
    core_u: tuple[float, ...]         # per-CPU empirical utilization (g_core_u_m / 65536)

    @property
    def requested_u(self) -> float:
        return self.u_permille / 1000.0

    @property
    def configured_u(self) -> float:
        # what the firmware set up (sum C_i/T_i in ticks), not a measurement
        return sum(c / p for c, p in zip(self.c, self.periods))

    @property
    def measured_u_per_task(self) -> tuple[float, ...]:
        return tuple((ci / ti) if ti else 0.0 for ci, ti in zip(self.ci_av, self.ti_av))

    @property
    def measured_u(self) -> float:
        return self.core_u[0] if self.core_u else 0.0


# --- ELF --------------------------------------------------------------------
def elf_symbols(elf: Path) -> dict[str, int]:
    out = subprocess.check_output(["arm-none-eabi-nm", str(elf)], cwd=ROOT, text=True)
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def require_symbols(symbols: dict[str, int], names: Sequence[str]) -> None:
    missing = [n for n in names if n not in symbols]
    if missing:
        raise RuntimeError("missing ELF symbols: " + ", ".join(missing))


def elf_cache_config(elf: Path) -> str:
    # Makefile records compile-time enables as absolute symbols (--defsym); nm -a lists them.
    out = subprocess.check_output(["arm-none-eabi-nm", "-a", str(elf)], cwd=ROOT, text=True)
    vals = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("A", "a") and parts[2].startswith("_cfg_enable_"):
            vals[parts[2]] = int(parts[0], 16)
    onoff = lambda v: "on" if vals.get(v, 0) else "off"
    return (f"MMU {onoff('_cfg_enable_mmu')} · D {onoff('_cfg_enable_dcache')} · "
            f"I {onoff('_cfg_enable_icache')} · L2 {onoff('_cfg_enable_l2')} · SMP {onoff('_cfg_enable_smp')}")


# --- OpenOCD ----------------------------------------------------------------
class OCD:
    def __init__(self, spawn_tries: int = 3):
        self.proc = None
        self.sock = None
        self.owns_daemon = False
        self.fault_addr: int | None = None   # g_fault[], set by main() for crash decode
        self.error_addr: int | None = None   # g_error_message[], set by main() for raise_error() strings
        self.ctx_addr: int | None = None     # g_error_ctx[], raise_error_ctx() context value
        self.edf_bad_addr: int | None = None # g_edf_bad_thread[..], harness watchdog report block
        self.num_cpus: int = 1

        # Reuse a running daemon (openocd/ocd) rather than spawn+SIGKILL churn, which
        # is the main cause of USB-Blaster wedging (stale libusb endpoints).
        if self._connect():
            return
        for attempt in range(spawn_tries):
            if attempt > 0:
                subprocess.run(["pkill", "-9", "openocd"], stderr=subprocess.DEVNULL)
                time.sleep(0.5)
            # start_new_session: put openocd in its own session so a terminal Ctrl-C
            # (SIGINT to the foreground process group) does NOT kill it -- Python still
            # gets KeyboardInterrupt, but the daemon stays up so diagnose() can read
            # both cores' PCs on interrupt instead of hitting a reset connection.
            self.proc = subprocess.Popen(["openocd", "-f", str(OPENOCD_CFG), "-c", "init"],
                                         cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                         start_new_session=True)
            self.owns_daemon = True
            for _ in range(30):
                if self.proc.poll() is not None:
                    break
                if self._connect():
                    return
                time.sleep(0.5)
            if self.proc.poll() is None:
                self.proc.kill()
        raise RuntimeError("OpenOCD/JTAG did not come up (USB-Blaster wedged? power-cycle it)")

    def _connect(self) -> bool:
        try:
            sock = socket.socket()
            sock.connect(("localhost", 6666))
            sock.settimeout(45)
            self.sock = sock
            self.cmd("cortex_a maskisr on")
            self.cmd("rbp all")          # clear stale breakpoints on reuse
            return True
        except (ConnectionRefusedError, OSError):
            self.sock = None
            return False

    def __enter__(self) -> "OCD":
        return self

    def __exit__(self, exc_type, exc, tb) -> bool:
        self.close()
        return False

    def cmd(self, command: str) -> str:
        if self.sock is None:
            raise RuntimeError("OpenOCD socket is closed")
        self.sock.sendall((command + "\x1a").encode())
        resp = b""
        while not resp.endswith(b"\x1a"):
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("OpenOCD connection closed")
            resp += chunk
        return resp[:-1].decode()

    # memory
    def read_words(self, addr: int, count: int) -> list[int]:
        out = self.cmd(f"mdw phys 0x{addr:08x} {count}")
        vals: list[int] = []
        for line in out.splitlines():
            if ":" in line:
                vals += [int(w, 16) for w in re.findall(r"\b[0-9a-fA-F]{8}\b", line.split(":", 1)[1])]
        if len(vals) < count:
            raise RuntimeError(f"short JTAG read at 0x{addr:08x}: wanted {count}, got {len(vals)}\n{out}")
        return vals[:count]

    def read_u32(self, addr: int) -> int:
        return self.read_words(addr, 1)[0]

    def read_bytes(self, addr: int, size: int) -> bytes:
        words = self.read_words(addr, (size + 3) // 4)
        return b"".join(w.to_bytes(4, "little") for w in words)[:size]

    def write_u32(self, addr: int, value: int) -> None:
        self.cmd(f"mww phys 0x{addr:08x} 0x{value:08x}")

    # run control
    def load_image(self, elf: Path, entry: int) -> None:
        self.cmd("halt")
        self.cmd(f"load_image {elf}")
        self.cmd("reg cpsr 0x1d3")
        self.cmd(f"reg pc 0x{entry:08x}")

    def resume(self) -> None:
        self.cmd("resume")

    def halt(self) -> None:
        self.cmd("halt")

    def step(self) -> None:
        self.cmd("step")

    def pc(self) -> int:
        # CPU0 control-flow read. Halt first: reg pc only yields on a halted core.
        self.cmd("halt")
        out = self.cmd("reg pc")
        m = re.search(r"0x([0-9a-fA-F]+)", out)
        if m is None:
            raise RuntimeError(f"cannot read PC -- core not halted or in reset "
                               f"(crash/WDT loop before the gate?): reg pc -> {out!r}")
        return int(m.group(1), 16)

    def dump_pcs(self) -> str:
        # Headshot + read PC on every core. A core in reset reports "unavailable".
        out = []
        for core, tgt in enumerate(["cv_hps.cpu", "cv_hps.cpu1"][:self.num_cpus]):
            self.cmd(f"targets {tgt}")
            self.cmd("halt")
            m = re.search(r"0x([0-9a-fA-F]+)", self.cmd("reg pc"))
            out.append(f"CPU{core}=" + (f"0x{int(m.group(1), 16):08x}" if m else "unavailable"))
        self.cmd("targets cv_hps.cpu")
        return ", ".join(out)

    def read_error_message(self, core: int) -> str | None:
        # g_error_message[core][FAULT_MSG_LEN], a C string written by raise_error().
        if self.error_addr is None:
            return None
        try:
            raw = self.read_bytes(self.error_addr + core * FAULT_MSG_LEN, FAULT_MSG_LEN)
        except Exception:
            return None
        text = raw.split(b"\x00", 1)[0]
        return text.decode("ascii", "replace") if text else None

    def read_fault(self) -> str | None:
        if self.fault_addr is None:
            return None
        try:
            words = self.read_words(self.fault_addr, 8 * self.num_cpus)
        except Exception:
            return None
        reports = []
        for core in range(self.num_cpus):
            r = fault_report(words[core * 8:core * 8 + 8], core)
            if not r:
                continue
            msg = self.read_error_message(core)
            if msg:
                r += f"\n  raised      = {msg}"
                if self.ctx_addr is not None:
                    try:
                        ctx = self.read_u32(self.ctx_addr + core * 4)
                        if ctx:
                            r += f"\n  ctx         = 0x{ctx:08x}"
                            if 0x00200000 <= ctx < 0x01200000:   # heap thread_t: dump its field block
                                fields = self.read_words(ctx + 0x2000, 16)   # ctx + THREAD_STACK_SIZE
                                r += "\n  thread_t fields (ctx+0x2000):\n    " + \
                                     " ".join(f"{w:08x}" for w in fields)
                    except Exception:
                        pass
            reports.append(r)

        # Harness watchdog detail (edf_flag_corruption): task#/core/expected/actual/tick.
        if self.edf_bad_addr is not None:
            try:
                b = self.read_words(self.edf_bad_addr, 6)   # thread, core, task, expected, actual, tick
                if b[0]:
                    reports.append(
                        f"*** EDF WATCHDOG ***\n"
                        f"  thread      = 0x{b[0]:08x}\n"
                        f"  core        = CPU{b[1]}\n"
                        f"  task index  = {b[2]}\n"
                        f"  expected    = 0x{b[3]:08x} ({b[3]})\n"
                        f"  actual      = 0x{b[4]:08x} ({b[4]})\n"
                        f"  at tick     = {b[5]}")
            except Exception:
                pass

        return "\n".join(reports) if reports else None

    def diagnose(self) -> str:
        # The one crash tap. ALWAYS every core's live PC; fault decode added on top,
        # never instead. You never get one core when the machine is angry.
        parts = [self.dump_pcs()]
        fault = self.read_fault()
        if fault:
            parts.append(fault)
        return "\n".join(parts)

    def wait_halt(self, timeout: float = 30.0) -> int:
        out = self.cmd(f"wait_halt {int(timeout * 1000)}").lower()
        if "timeout" in out or "timed out" in out:
            raise RuntimeError(f"no halt in {timeout:.0f}s: {self.diagnose()}")
        return self.pc()

    def add_hw_breakpoint(self, addr: int) -> None:
        out = self.cmd(f"bp 0x{addr:08x} 4 hw").lower()
        if "error" in out or "resource not available" in out:
            raise RuntimeError(f"cannot install hw breakpoint at 0x{addr:08x}:\n{out}")

    def remove_hw_breakpoint(self, addr: int) -> None:
        self.cmd(f"rbp 0x{addr:08x}")

    def expect_breakpoint(self, expected_addr: int, *, timeout: float = 30.0) -> None:
        pc = self.wait_halt(timeout)
        if pc != expected_addr:
            raise RuntimeError(f"halt 0x{pc:08x} != expected 0x{expected_addr:08x}: {self.diagnose()}")

    def continue_from_breakpoint(self) -> None:
        # OpenOCD steps over the managed hw breakpoint, then we continue.
        self.step()
        self.resume()

    def drain(self) -> None:
        # Clear any bytes left in the socket from a command whose recv was cut short
        # (e.g. Ctrl-C mid-cmd), so a following cmd() reads its own reply and not a
        # stale one. Best-effort; used before diagnosing on interrupt.
        if self.sock is None:
            return
        self.sock.setblocking(False)
        try:
            while self.sock.recv(4096):
                pass
        except (BlockingIOError, OSError):
            pass
        finally:
            self.sock.setblocking(True)
            self.sock.settimeout(45)

    def close(self) -> None:
        if self.sock is not None:
            if self.owns_daemon:                    # leave a reused external daemon running
                try: self.cmd("shutdown")
                except Exception: pass
            try: self.sock.close()
            except Exception: pass
            self.sock = None
        if self.owns_daemon and self.proc is not None and self.proc.poll() is None:
            self.proc.kill()


# --- telemetry collection ---------------------------------------------------
def read_metric(ocd: OCD, base: int, index: int) -> Metric:
    minimum, maximum, total, count = METRIC_STRUCT.unpack(
        ocd.read_bytes(base + index * METRIC_SIZE, METRIC_SIZE))
    name = ALLOC_NAMES[index] if index < len(ALLOC_NAMES) else f"metric_{index}"
    return Metric(name, count, total, minimum, maximum)


def collect_alloc(ocd: OCD, base: int) -> tuple[Metric, ...]:
    return tuple(read_metric(ocd, base, i) for i in range(ALLOC_METRICS))


def collect_alloc_samples(ocd: OCD, base: int) -> dict[str, list[int]]:
    # g_alloc_samples is row-major [op][iter]; slice one op's ALLOC_ITERS run at a time.
    words = ocd.read_words(base, len(ALLOC_SAMPLE_OPS) * ALLOC_SAMPLE_ITERS)
    samples: dict[str, list[int]] = {}
    for op_index, op_name in enumerate(ALLOC_SAMPLE_OPS):
        start = op_index * ALLOC_SAMPLE_ITERS
        samples[op_name] = words[start:start + ALLOC_SAMPLE_ITERS]
    return samples


def collect_rmw_samples(ocd: OCD, base: int) -> dict[str, list[int]]:
    # g_rmw_samples is row-major [2][RMW_ROW_LEN]; each row holds RMW_ROW_PASSES[r]
    # valid passes (the 256KB row is shorter, trailing entries stay zero).
    words = ocd.read_words(base, 2 * RMW_ROW_LEN)
    samples: dict[str, list[int]] = {}
    for row in range(2):
        start = row * RMW_ROW_LEN
        samples[RMW_LABELS[row]] = words[start:start + RMW_ROW_PASSES[row]]
    return samples


def collect_matmul_samples(ocd: OCD, base: int) -> dict[str, list[int]]:
    return {"matmul_32": ocd.read_words(base, MATMUL_REPS)}


def expected_jobs(ticks: int, period: int) -> int:
    # First release is on tick 1: 1, 1+T, 1+2T, ...
    return ((ticks - 1) // period) + 1 if ticks else 0


def collect_edf_trial(ocd: OCD, symbols: dict[str, int], periods: tuple[int, ...], num_cpus: int) -> EDFResult:
    ticks = ocd.read_u32(symbols["g_ticks_m"])
    raw = ocd.read_bytes(symbols["g_edf_metrics"], len(periods) * EDF_METRIC_SIZE)
    mets = [EDF_METRIC_STRUCT.unpack_from(raw, i * EDF_METRIC_SIZE) for i in range(len(periods))]
    return EDFResult(
        index=ocd.read_u32(symbols["g_edf_u_index"]),
        u_permille=ocd.read_u32(symbols["g_edf_u_permille"]),
        ticks=ticks,
        misses=ocd.read_u32(symbols["g_misses_m"]),
        c=tuple(ocd.read_words(symbols["g_edf_C"], len(periods))),
        periods=periods,
        done=tuple(ocd.read_words(symbols["g_edf_done"], len(periods))),
        expected=tuple(expected_jobs(ticks, p) for p in periods),
        ci=tuple(m[0] for m in mets),
        ci_av=tuple(m[1] for m in mets),
        ti_av=tuple(m[5] for m in mets),
        sched_overhead=tuple(ocd.read_words(symbols["g_overhead_m"], num_cpus)),
        core_u=tuple(v / 65536.0 for v in ocd.read_words(symbols["g_core_u_m"], num_cpus)),
    )


# --- reporting --------------------------------------------------------------
def print_alloc(metrics: Sequence[Metric]) -> None:
    print("[allocbench]")
    for m in metrics:
        print(f"    {m.name:<14} n={m.count:<5} mean={m.mean:<6} min={m.minimum:<6} max={m.maximum}")


def print_edf_header() -> None:
    print(f"\n{'reqU':>6} {'cfgU':>6} {'measU':>6} {'ticks':>8} {'miss':>8} "
          f"{'sched c/c':>10}   per-core U")


def print_edf_result(r: EDFResult) -> None:
    per_core = "  ".join(f"c{i}={u:.3f}" for i, u in enumerate(r.core_u))
    print(f"{r.requested_u:>6.3f} {r.configured_u:>6.3f} {r.measured_u:>6.3f} {r.ticks:>8} "
          f"{r.misses:>8} {'/'.join(map(str, r.sched_overhead)):>10}   {per_core}")


# --- CSV --------------------------------------------------------------------
def write_alloc_csv(path: Path, metrics: Sequence[Metric]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["op", "count", "mean_cyc", "min_cyc", "max_cyc"])
        for m in metrics:
            w.writerow([m.name, m.count, m.mean, m.minimum, m.maximum])


def write_samples_csv(path: Path, series: dict[str, list[int]]) -> None:
    # One column per series, one row per iteration -- the raw data behind a warm-up
    # plot. Series may differ in length (e.g. the two RMW sweeps); shorter ones are
    # padded with blanks.
    names = list(series)
    rows = max((len(v) for v in series.values()), default=0)
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["iter", *names])
        for i in range(rows):
            w.writerow([i, *(series[n][i] if i < len(series[n]) else "" for n in names)])


def write_cache_csv(path: Path, metrics: Sequence[Metric]) -> None:
    # The RMW + matmul workloads (the cache-sensitive ones). For the RMW sweeps,
    # min_cyc = warm (cache-hit) pass, max_cyc = cold (fill) pass.
    by_name = {m.name: m for m in metrics}
    cache_config = elf_cache_config(TEST_ELF)
    with path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["workload", "cache_config", "count", "mean_cyc", "min_cyc", "max_cyc"])
        for name in ("mem_walk_8k", "mem_walk_256k", "matmul_32"):
            m = by_name.get(name)
            if m:
                writer.writerow([m.name, cache_config, m.count, m.mean, m.minimum, m.maximum])


def write_edf_csv(path: Path, rows: Sequence[EDFResult]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["requested_u", "configured_u", "measured_u", "ticks", "misses",
                    "sched_overhead_c0_cyc", "sched_overhead_c1_cyc",
                    "C0", "C1", "C2", "T0", "T1", "T2", "done0", "done1", "done2",
                    "expected0", "expected1", "expected2",
                    "ci_av0", "ci_av1", "ci_av2", "ti_av0", "ti_av1", "ti_av2"])
        for r in rows:
            w.writerow([r.requested_u, round(r.configured_u, 6), round(r.measured_u, 6),
                        r.ticks, r.misses,
                        r.sched_overhead[0], r.sched_overhead[1] if len(r.sched_overhead) > 1 else 0,
                        *r.c, *r.periods, *r.done, *r.expected, *r.ci_av, *r.ti_av])


def write_balance_csv(path: Path, bal: BalanceResult) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["id", "core", "period"])
        for i in range(len(bal.assign)):
            w.writerow([i, bal.assign[i], bal.periods[i]])


def _write_micro_csv(path: Path, rows: Sequence[tuple[str, Sequence[int]]], extra: Sequence = ()) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["distribution", "count", "min", "mean", "median", "p99", "max", "unit"])
        for name, v in rows:
            s = dist_stats(v)
            w.writerow([name, s["count"], s["min"], round(s["mean"], 1),
                        s["median"], s["p99"], s["max"], "cycles"])
        for row in extra:
            w.writerow(row)


# --- plots ------------------------------------------------------------------
@dataclass(frozen=True)
class Sweep:
    x:     tuple[int, ...]       # x-axis: payload size (bytes) or free-list length (holes)
    alloc: tuple[Metric, ...]    # per x: alloc cost distribution (cycles)
    free:  tuple[Metric, ...]    # per x: free cost distribution (cycles)


def collect_sweep(ocd: OCD, symbols: dict[str, int],
                  n_sym: str, x_sym: str, alloc_sym: str, free_sym: str) -> Sweep:
    # Generic alloc/free sweep reader. Identical symbol layout in the normal (TLSF) and
    # FreeRTOS (heap_4) binaries, so test.py and test_freertos.py share it.
    n = min(ocd.read_u32(symbols[n_sym]), 64)
    x = tuple(ocd.read_words(symbols[x_sym], n))
    alloc = tuple(read_metric(ocd, symbols[alloc_sym], i) for i in range(n))
    free  = tuple(read_metric(ocd, symbols[free_sym], i) for i in range(n))
    return Sweep(x, alloc, free)


def write_sweep_csv(path: Path, sweep: Sweep, xname: str) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow([xname, "alloc_mean", "alloc_min", "alloc_max",
                    "free_mean", "free_min", "free_max", "unit"])
        for i, xv in enumerate(sweep.x):
            a, fr = sweep.alloc[i], sweep.free[i]
            w.writerow([xv, a.mean, a.minimum, a.maximum, fr.mean, fr.minimum, fr.maximum, "cycles"])


def plot_sweep(sweep: Sweep, path: Path, title: str, xlabel: str) -> None:
    # alloc and free cost vs the swept x (mean line + min as the clean lower bound).
    # Log-2 x since both sweeps are geometric.
    x = sweep.x
    fig, ax = plt.subplots(figsize=(10, 5.2))
    ax.plot(x, [m.mean for m in sweep.alloc], "o-", color="#1f77b4", label="alloc (mean)")
    ax.plot(x, [m.minimum for m in sweep.alloc], ":", color="#1f77b4", alpha=0.6, label="alloc (min)")
    ax.plot(x, [m.mean for m in sweep.free], "s-", color="#2ca02c", label="free (mean)")
    ax.plot(x, [m.minimum for m in sweep.free], ":", color="#2ca02c", alpha=0.6, label="free (min)")
    ax.set_xscale("log", base=2)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("cycles")
    ax.set_title(title)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=9)
    fig.tight_layout()
    fig.savefig(path, dpi=140)
    plt.close(fig)


# per sweep: (n_sym, x_sym, alloc_sym, free_sym, csv_xname, title_suffix, xlabel, file_stem)
ALLOC_SWEEPS = {
    "size": ("g_allocsweep_n", "g_allocsweep_sizes", "g_allocsweep_alloc", "g_allocsweep_free",
             "size_bytes", "vs payload size", "payload size (bytes)", "allocsweep"),
    "frag": ("g_fragsweep_n", "g_fragsweep_levels", "g_fragsweep_alloc", "g_fragsweep_free",
             "free_list_len", "vs fragmentation (free-list length)", "free-list length (holes)", "fragsweep"),
}


def plot_alloc(metrics: Sequence[Metric], path: Path) -> None:
    names  = [m.name for m in metrics]
    means  = [m.mean for m in metrics]
    lo_err = [m.mean - m.minimum for m in metrics]
    hi_err = [m.maximum - m.mean for m in metrics]
    fig, ax = plt.subplots(figsize=(7, 4))
    ax.bar(range(len(metrics)), means, yerr=[lo_err, hi_err], capsize=4)
    ax.set_xticks(range(len(metrics)))
    ax.set_xticklabels(names, rotation=12)
    ax.set_ylabel("cycles")
    ax.set_title(f"Allocator operation cost  ({elf_cache_config(TEST_ELF)})")
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def plot_warmup(series: dict[str, list[int]], path: Path, title: str,
                xlabel: str = "iteration") -> None:
    # Cycles vs iteration index, one line per series. The allocator/cache workloads
    # are near-deterministic once warm, so the story is the cold-start transient
    # (first iteration cold-fills cache); a log-y line plot shows it far better
    # than a histogram, where 99%+ of the mass collapses onto one bar.
    fig, ax = plt.subplots(figsize=(9, 5))
    for name, values in series.items():
        if not values:
            continue
        # markers help on short (per-pass/per-rep) series; long runs read as a line.
        marker = "o" if len(values) <= 64 else None
        ax.plot(range(len(values)), values, marker=marker, ms=4, linewidth=1.2, label=name)
    ax.set_yscale("log")
    ax.set_xlabel(xlabel)
    ax.set_ylabel("cycles (log)")
    ax.set_title(f"{title}  ({elf_cache_config(TEST_ELF)})")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3, which="both")
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def plot_cache(metrics: Sequence[Metric], path: Path) -> None:
    # Left: RMW sweep, 8192-byte set (fits L1) and 262144-byte set (fits L2), two
    # bars each -- cold = slowest pass (max), warm = fastest pass (min). Right: matmul.
    by_name = {m.name: m for m in metrics}
    matmul = by_name.get("matmul_32")
    fig, (ax_rmw, ax_mm) = plt.subplots(1, 2, figsize=(9, 4))

    COLD_COLOR = "#c0504d"
    WARM_COLOR = "#4f81bd"

    bar_x, bar_height, bar_color = [], [], []
    tick_x, tick_label = [], []
    x_cursor = 0.0
    for size_label, name in (("8 KB", "mem_walk_8k"), ("256 KB", "mem_walk_256k")):
        m = by_name.get(name)
        if m is None:
            continue
        # cold bar at x_cursor, warm bar one unit to its right.
        bar_x      += [x_cursor, x_cursor + 1.0]
        bar_height += [m.maximum, m.minimum]
        bar_color  += [COLD_COLOR, WARM_COLOR]
        tick_x     += [x_cursor, x_cursor + 1.0]
        tick_label += [f"{size_label}\ncold", f"{size_label}\nwarm"]
        x_cursor   += 2.7                              # gap before the next size's pair
    if bar_x:
        ax_rmw.bar(bar_x, bar_height, width=0.8, color=bar_color)
        ax_rmw.set_yscale("log")
        ax_rmw.set_xticks(tick_x)
        ax_rmw.set_xticklabels(tick_label, fontsize=8)
        ax_rmw.set_title("SDRAM RMW / pass: cold vs warm")
    ax_rmw.set_ylabel("cycles (log)")
    ax_rmw.grid(axis="y", alpha=0.3, which="both")
    if matmul is not None:
        ax_mm.bar([matmul.name], [matmul.mean],
                  yerr=[[matmul.mean - matmul.minimum], [matmul.maximum - matmul.mean]],
                  capsize=6, color="#9bbb59")
        ax_mm.set_title(f"{matmul.name}: compute time-to-complete")
    ax_mm.set_ylabel("cycles")
    ax_mm.grid(axis="y", alpha=0.3)
    fig.suptitle(f"Cache-sensitive workloads  ({elf_cache_config(TEST_ELF)})")
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def plot_edf(rows: Sequence[EDFResult], path: Path) -> None:
    requested = [r.requested_u for r in rows]
    misses    = [r.misses for r in rows]
    measured  = [r.measured_u for r in rows]
    n_cpus    = len(rows[0].sched_overhead) if rows else 1
    overhead  = [[r.sched_overhead[c] for r in rows] for c in range(n_cpus)]
    fig, (a1, a2, a3) = plt.subplots(3, 1, figsize=(8, 10), sharex=True)
    a1.plot(requested, misses, "o-")
    a1.axvline(1.0, linestyle="--")
    a1.set_ylabel("deadline misses")
    a1.set_title("EDF: schedulability, scheduler cost, measured utilization")
    a1.grid(alpha=0.3)
    for c in range(n_cpus):
        a2.plot(requested, overhead[c], "o-", label=f"CPU{c}")
    a2.axvline(1.0, linestyle="--")
    a2.set_ylabel("scheduler cycles / tick (EWMA)")
    a2.legend()
    a2.grid(alpha=0.3)
    if requested:
        lo, hi = min(requested), max(requested)
        a3.plot([lo, hi], [lo, hi], "--", color="gray", label="y = x")
    a3.plot(requested, measured, "o-", label="measured")
    a3.axvline(1.0, linestyle="--")
    a3.set_ylabel("measured utilization")
    a3.set_xlabel("requested utilization")
    a3.legend()
    a3.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def plot_gantt(trace: Sequence[int], periods: Sequence[int], path: Path, u_permille: int,
               u_configured: float = 0.0, task_u: Sequence[float] = (), label: str = "",
               window: int = GANTT_WINDOW) -> None:
    n = min(len(trace), window)
    nt = len(periods)
    colors = ["#1f77b4", "#ff7f0e", "#2ca02c"]
    width = min(26.0, max(12.0, n / 130.0))
    fig, ax = plt.subplots(figsize=(width, 2.6))
    for i in range(nt):
        y = nt - 1 - i                                   # task 0 at top
        segs, t = [], 0
        while t < n:
            if trace[t] == i:
                s = t
                while t < n and trace[t] == i:
                    t += 1
                segs.append((s, t - s))
            else:
                t += 1
        ax.broken_barh(segs, (y + 0.15, 0.7), facecolors=colors[i % len(colors)])
        for k in range(0, n, periods[i]):                # release arrows
            ax.annotate("", xy=(k, y + 1.0), xytext=(k, y + 0.12),
                        arrowprops=dict(arrowstyle="->", color="black", lw=0.7))
    row_label = lambda i: f"$\\tau_{i}$ (T={periods[i]})" + (f"\nU={task_u[i]:.3f}" if i < len(task_u) else "")
    measured_total = sum(task_u) if task_u else 0.0
    ax.set_yticks([nt - 1 - i + 0.5 for i in range(nt)])
    ax.set_yticklabels([row_label(i) for i in range(nt)], fontsize=8)
    ax.set_ylim(0, nt)
    ax.set_xlim(0, n)
    ax.set_xlabel("time (ticks)")
    ax.set_title(f"EDF schedule  (Ureq={u_permille / 1000:.3f}, Ucfg={u_configured:.3f}, "
                 f"Umeas={measured_total:.3f}{', ' + label if label else ''}; first {n} ticks; up-arrow = release)")
    ax.grid(axis="x", alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def plot_balance(bal: BalanceResult, path: Path, num_cpus: int,
                 window: int = GANTT_WINDOW) -> None:
    # One figure, one Gantt per core. Each core's rows are the ids the balancer
    # assigned to it (from add_thread's returned thread_t->core), labelled by id.
    colors = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b"]
    per_core = [[i for i in range(len(bal.assign)) if bal.assign[i] == c] for c in range(num_cpus)]
    width = min(26.0, max(12.0, window / 130.0))
    fig, axes = plt.subplots(num_cpus, 1, figsize=(width, 2.6 * num_cpus), squeeze=False)

    for c in range(num_cpus):
        ax = axes[c][0]
        ids = per_core[c]
        nr = len(ids)
        trace = bal.traces[c] if c < len(bal.traces) else b""
        n = min(len(trace), window)
        for row, tid in enumerate(ids):
            y = nr - 1 - row                             # first id at top
            segs, t = [], 0
            while t < n:
                if trace[t] == tid:
                    s = t
                    while t < n and trace[t] == tid:
                        t += 1
                    segs.append((s, t - s))
                else:
                    t += 1
            ax.broken_barh(segs, (y + 0.15, 0.7), facecolors=colors[tid % len(colors)])
            for k in range(0, n, bal.periods[tid]):      # release arrows
                ax.annotate("", xy=(k, y + 1.0), xytext=(k, y + 0.12),
                            arrowprops=dict(arrowstyle="->", color="black", lw=0.7))
        ax.set_yticks([nr - 1 - row + 0.5 for row in range(nr)])
        ax.set_yticklabels([f"id={tid} (T={bal.periods[tid]})" for tid in ids], fontsize=8)
        ax.set_ylim(0, max(nr, 1))
        ax.set_xlim(0, max(n, 1))
        ax.set_xlabel("time (ticks)")
        ax.set_title(f"CPU{c} load-balanced  (Umeas={bal.core_u[c]:.3f}, {nr} tasks; "
                     f"first {n} ticks; up-arrow = release)")
        ax.grid(axis="x", alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def _pct(sorted_v: Sequence[int], q: float) -> int:
    if not sorted_v:
        return 0
    i = min(len(sorted_v) - 1, max(0, int(round(q * (len(sorted_v) - 1)))))
    return sorted_v[i]


def _median(sorted_v: Sequence[int]) -> float:
    n = len(sorted_v)
    if not n:
        return 0.0
    mid = n // 2
    # true median: average the two middle values for even n, not a nearest-rank pick.
    return float(sorted_v[mid]) if n % 2 else (sorted_v[mid - 1] + sorted_v[mid]) / 2.0


def dist_stats(v: Sequence[int]) -> dict[str, float]:
    s = sorted(v)
    n = len(s)
    if not n:
        return {"count": 0, "min": 0, "mean": 0.0, "median": 0, "p99": 0, "max": 0}
    return {"count": n, "min": s[0], "mean": sum(s) / n,
            "median": _median(s), "p99": _pct(s, 0.99), "max": s[-1]}


def _hist(ax, v: Sequence[int], title: str, color: str) -> None:
    # One bar per integer cycle value across the observed range -- a true count
    # histogram, no binning, no clipping, no overlay lines. Stats go in the title.
    s = dist_stats(v)
    if v:
        lo, hi = min(v), max(v)
        ax.hist(v, bins=range(lo, hi + 2), color=color)   # edges lo..hi+1 -> one bar per cycle
    ax.set_title(f"{title}  (n={s['count']}, min={s['min']}, mean={s['mean']:.1f}, "
                 f"p99={int(s['p99'])}, max={s['max']} cyc)", fontsize=9)
    ax.set_xlabel("cycles")
    ax.set_ylabel("count")
    ax.grid(axis="y", alpha=0.3)


def plot_ctxsw(res: MicroResult, path: Path) -> None:
    total = res.switch_count + res.resume_count
    p = res.switch_count / total if total else 0.0
    fig, axes = plt.subplots(2, 1, figsize=(10, 6))
    _hist(axes[0], res.ctxsw_switch, "next_thread -- switch (different thread ran)", "#1f77b4")
    _hist(axes[1], res.ctxsw_resume, "next_thread -- resume (same thread continued)", "#2ca02c")
    fig.suptitle(f"Context-switch cost  (switch fraction p = {p:.3f} for this taskset)", fontsize=11)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def plot_irqlat(res: MicroResult, path: Path) -> None:
    fig, ax = plt.subplots(1, 1, figsize=(10, 3.4))
    _hist(ax, res.irqlat, "IRQ dispatch latency -- SGI trigger to ISR body", "#ff7f0e")
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def plot_mutex(res: MicroResult, path: Path) -> None:
    fig, axes = plt.subplots(2, 1, figsize=(10, 6))
    _hist(axes[0], res.mutex_unc, "mutex -- lock+unlock, uncontended (one core)", "#9467bd")
    _hist(axes[1], res.mutex_con, "mutex -- acquire latency, contended (CPU1 holds)", "#d62728")
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def plot_memlat(res: MicroResult, path: Path) -> None:
    series = [("SDRAM read", res.memlat_sdram_rd), ("SDRAM write", res.memlat_sdram_wr),
              ("OCRAM read", res.memlat_ocram_rd), ("OCRAM write", res.memlat_ocram_wr)]
    fig, axes = plt.subplots(2, 2, figsize=(12, 7))
    for ax, (title, v) in zip(axes.flat, series):
        lo, hi = min(v), max(v)
        pad = max(3, (hi - lo) // 10)               # frame each panel to its own data
        ax.hist(v, bins=range(lo, hi + 2))          # bar width always 1 cycle
        ax.set_xlim(lo - pad, hi + pad)
        ax.xaxis.set_major_locator(MaxNLocator(integer=True))
        ax.set_title(title)
        ax.set_xlabel("cycles")
        ax.set_ylabel("count")
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def plot_suspend(res: SuspendResult, path: Path) -> None:
    # Two raster rows of the ticks each task actually ran at; the shaded band is
    # the requested suspension window, which should contain no run marks.
    fig, axes = plt.subplots(2, 1, figsize=(10, 4.2), sharex=False)

    def row(ax, title, run_ticks, start, width):
        if run_ticks:
            ax.eventplot([list(run_ticks)], colors="#1f77b4", lineoffsets=0, linelengths=0.8)
        if start:
            ax.axvspan(start, start + width, color="#d62728", alpha=0.18,
                       label=f"suspended {width} ticks")
            ax.legend(loc="upper right", fontsize=8)
        ax.set_title(title, fontsize=10)
        ax.set_yticks([])
        ax.set_xlabel("scheduler tick")

    # self: the suspension starts at the activation that called suspend_thread.
    self_start = res.self_ticks[3] if len(res.self_ticks) > 3 else 0
    row(axes[0], "self-suspend: suspender run ticks", res.self_ticks, self_start, res.self_requested)
    row(axes[1], "suspend-another: victim run ticks", res.other_ticks,
        res.other_suspend_tick, res.other_requested)

    fig.suptitle("suspend_thread -- a run gap of the requested width at the suspension point",
                 fontsize=11)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


# --- phases -----------------------------------------------------------------
def allocbench(ocd: OCD, symbols: dict[str, int]) -> tuple[list[Metric], dict[str, dict[str, list[int]]]]:
    bp = symbols["ktrace_bp_alloc_done"]
    ocd.add_hw_breakpoint(bp)
    ocd.resume()
    ocd.expect_breakpoint(bp, timeout=30.0)
    metrics = collect_alloc(ocd, symbols["g_metrics"])
    # All three per-iteration sample buffers are populated by this single breakpoint.
    warmup = {
        "alloc":  collect_alloc_samples(ocd, symbols["g_alloc_samples"]),
        "rmw":    collect_rmw_samples(ocd, symbols["g_rmw_samples"]),
        "matmul": collect_matmul_samples(ocd, symbols["g_matmul_samples"]),
    }
    print_alloc(metrics)

    # Allocator sweeps: alloc/free cost vs payload size, and vs free-list length. Same
    # symbols in the FreeRTOS binary (read there by test_freertos.py).
    sweeps: dict[str, Sweep] = {}
    for key, (n_sym, x_sym, a_sym, f_sym, *_rest) in ALLOC_SWEEPS.items():
        if symbols.get(n_sym):
            sweeps[key] = collect_sweep(ocd, symbols, n_sym, x_sym, a_sym, f_sym)

    ocd.remove_hw_breakpoint(bp)   # one-shot; free the HW slot for the micro phase (A9 has 6)
    return list(metrics), warmup, sweeps


def edf_test(ocd: OCD, symbols: dict[str, int], num_cpus: int,
             periods: tuple[int, ...], u_values: tuple[int, ...]
             ) -> tuple[list[EDFResult], dict[int, bytes], dict[int, bytes]]:
    ocd.continue_from_breakpoint()          # off bp_alloc (or load); ready/done armed by main()
    bp_edf_ready = symbols["ktrace_bp_edf_ready"]
    bp_edf_done  = symbols["ktrace_bp_edf_done"]
    print_edf_header()

    edf_results: list[EDFResult] = []
    traces: dict[int, bytes] = {}        # CPU0 schedule trace per trial (u_permille -> bytes)
    cpu1_traces: dict[int, bytes] = {}   # CPU1 schedule trace per trial (both cores run concurrently)

    for trial, expected_u in enumerate(u_values):
        ocd.expect_breakpoint(bp_edf_ready, timeout=15.0)
        idx = ocd.read_u32(symbols["g_edf_u_index"])
        u   = ocd.read_u32(symbols["g_edf_u_permille"])
        if idx != trial or u != expected_u:
            raise RuntimeError(f"EDF desync: expected trial {trial} U={expected_u}, got trial {idx} U={u}")

        ocd.continue_from_breakpoint()
        time.sleep(EDF_SECONDS)
        ocd.halt()

        if ocd.read_fault():
            raise RuntimeError(ocd.diagnose())

        result = collect_edf_trial(ocd, symbols, periods, num_cpus)
        edf_results.append(result)
        print_edf_result(result)

        # Both cores run this trial concurrently, so grab both schedule traces from
        # the same halt: g_sched_trace[core][.], g_trace_len[core]. The trace region
        # is uncached and strongly-ordered, so a phys read sees a consistent prefix
        # without halting CPU1 -- a trace_len of N means bytes [0,N) are committed.
        n0 = min(ocd.read_u32(symbols["g_trace_len"]), TRACE_TICKS)
        if n0:
            traces[result.u_permille] = ocd.read_bytes(symbols["g_sched_trace"], n0)
        if num_cpus > 1:
            n1 = min(ocd.read_u32(symbols["g_trace_len"] + 4), TRACE_TICKS)   # g_trace_len[1]
            if n1:
                cpu1_traces[result.u_permille] = ocd.read_bytes(symbols["g_sched_trace"] + TRACE_TICKS, n1)

        # Release the sampled trial; let firmware run on to the next trial's bp_edf_ready.
        # It may already be sitting on it (short trial) -- only resume if it isn't.
        ocd.write_u32(symbols["g_test_release"], 1)
        ocd.resume()
        time.sleep(0.05)
        ocd.halt()
        if ocd.pc() != bp_edf_ready:
            ocd.resume()
        ocd.resume()

    ocd.expect_breakpoint(bp_edf_done, timeout=30.0)

    return edf_results, traces, cpu1_traces


@dataclass(frozen=True)
class BalanceResult:
    assign: tuple[int, ...]           # id -> assigned core
    periods: tuple[int, ...]          # id -> period (ticks)
    core_u: tuple[float, ...]         # per-CPU empirical utilization (g_core_u_m / 65536)
    traces: tuple[bytes, ...]         # per-CPU schedule trace (ids; 0xFF = idle)


def balance_test(ocd: OCD, symbols: dict[str, int], num_cpus: int) -> BalanceResult:
    bp_ready = symbols["ktrace_bp_balance_ready"]
    bp_done  = symbols["ktrace_bp_balance_done"]

    # Firmware is parked on bp_edf_done (sweep complete). Step it on into the
    # load-balance routine: it builds the six tasks via add_thread, records each
    # one's assigned core (g_bal_assign), then stops at bp_balance_ready with
    # interrupts still off -- nothing has run yet, so the assignment is final.
    ocd.continue_from_breakpoint()
    ocd.expect_breakpoint(bp_ready, timeout=15.0)

    assign  = tuple(ocd.read_words(symbols["g_bal_assign"], BAL_TASKS))
    periods = tuple(ocd.read_words(symbols["g_bal_periods"], BAL_TASKS))

    # Let both cores run the balanced set for a measurement window.
    ocd.continue_from_breakpoint()
    time.sleep(EDF_SECONDS)
    ocd.halt()

    if ocd.read_fault():
        raise RuntimeError(ocd.diagnose())

    core_u = tuple(v / 65536.0 for v in ocd.read_words(symbols["g_core_u_m"], num_cpus))

    traces: list[bytes] = []
    for c in range(num_cpus):
        n = min(ocd.read_u32(symbols["g_trace_len"] + 4 * c), TRACE_TICKS)   # g_trace_len[c]
        traces.append(ocd.read_bytes(symbols["g_sched_trace"] + TRACE_TICKS * c, n) if n else b"")

    print("\nload balance:")
    for i in range(BAL_TASKS):
        print(f"  id={i} (T={periods[i]}) -> CPU{assign[i]}")
    print("  " + "  ".join(f"U(CPU{c})={core_u[c]:.3f}" for c in range(num_cpus)))

    # Release the window; firmware kills its tasks, tears down, stops at done.
    ocd.write_u32(symbols["g_test_release"], 1)
    ocd.resume()
    ocd.expect_breakpoint(bp_done, timeout=30.0)

    return BalanceResult(assign, periods, core_u, tuple(traces))


@dataclass(frozen=True)
class MicroResult:
    ctxsw_switch: tuple[int, ...]     # per-tick next_thread cost, cycles, switch ticks
    ctxsw_resume: tuple[int, ...]     # ... resume (same-thread) ticks
    switch_count: int                 # full counts (unbounded) -> p = switch/(switch+resume)
    resume_count: int
    irqlat: tuple[int, ...]           # SGI trigger -> ISR body, cycles
    mutex_unc: tuple[int, ...]        # lock+unlock uncontended, cycles
    mutex_con: tuple[int, ...]        # acquire latency while CPU1 holds, cycles
    memlat_sdram_rd: tuple[int, ...]  # uncached single-word READ latency, SDRAM, cycles
    memlat_sdram_wr: tuple[int, ...]  # uncached single-word WRITE latency, SDRAM, cycles
    memlat_ocram_rd: tuple[int, ...]  # uncached single-word READ latency, OCRAM, cycles
    memlat_ocram_wr: tuple[int, ...]  # uncached single-word WRITE latency, OCRAM, cycles
    memlat_baseline: int              # empty-window pmu+dsb overhead, cycles (common-mode offset)


def micro_test(ocd: OCD, symbols: dict[str, int]) -> MicroResult:
    bp_done = symbols["ktrace_bp_micro_done"]
    ocd.add_hw_breakpoint(bp_done)   # armed late: alloc_done was freed, so this is the 6th slot

    # Firmware is parked on bp_balance_done. The whole micro suite (ctxsw, irqlat,
    # mutex, memlat) runs free and self-contained. Poll rather than one long wait_halt
    # (which would blow the 45s socket timeout): halt every couple seconds, and if
    # it hasn't finished, read g_micro_phase so a stall names the failing phase.
    bp_fault = symbols["fault_trap"]
    ocd.continue_from_breakpoint()

    deadline = time.time() + 90.0
    while True:
        time.sleep(2.0)
        ocd.halt()
        pc = ocd.pc()
        if pc == bp_done:
            break
        if pc == bp_fault:
            raise RuntimeError(ocd.diagnose())
        phase = ocd.read_u32(symbols["g_micro_phase"])
        if time.time() > deadline:
            raise RuntimeError(f"micro stalled in phase {phase} "
                               f"(1=ctxsw 2=irqlat 3=mutex 4=memlat 5=done), pc=0x{pc:08x}:\n{ocd.diagnose()}")
        print(f"  micro running: phase={phase} pc=0x{pc:08x}")
        ocd.resume()

    switch_count = ocd.read_u32(symbols["g_ctxsw_switch_count"])
    resume_count = ocd.read_u32(symbols["g_ctxsw_resume_count"])
    n_sw = min(switch_count, CTXSW_SAMPLES)
    n_rs = min(resume_count, CTXSW_SAMPLES)

    # 0xFFFFFFFF marks a sample the firmware gave up on (guard/cap expired): an IRQ
    # ping never seen, or a contended acquire that never won. Drop and count them.
    irqlat_raw = ocd.read_words(symbols["g_irqlat"], IRQ_SAMPLES)
    irqlat = tuple(x for x in irqlat_raw if x != 0xFFFFFFFF)
    irq_lost = len(irqlat_raw) - len(irqlat)

    con_raw = ocd.read_words(symbols["g_mutex_con"], MUTEX_SAMPLES)
    mutex_con = tuple(x for x in con_raw if x != 0xFFFFFFFF)
    con_lost = len(con_raw) - len(mutex_con)

    res = MicroResult(
        ctxsw_switch=tuple(ocd.read_words(symbols["g_ctxsw_switch"], n_sw)) if n_sw else (),
        ctxsw_resume=tuple(ocd.read_words(symbols["g_ctxsw_resume"], n_rs)) if n_rs else (),
        switch_count=switch_count,
        resume_count=resume_count,
        irqlat=irqlat,
        mutex_unc=tuple(ocd.read_words(symbols["g_mutex_unc"], MUTEX_SAMPLES)),
        mutex_con=mutex_con,
        memlat_sdram_rd=tuple(ocd.read_words(symbols["g_memlat_sdram_rd"], MEMLAT_SAMPLES)),
        memlat_sdram_wr=tuple(ocd.read_words(symbols["g_memlat_sdram_wr"], MEMLAT_SAMPLES)),
        memlat_ocram_rd=tuple(ocd.read_words(symbols["g_memlat_ocram_rd"], MEMLAT_SAMPLES)),
        memlat_ocram_wr=tuple(ocd.read_words(symbols["g_memlat_ocram_wr"], MEMLAT_SAMPLES)),
        memlat_baseline=ocd.read_u32(symbols["g_memlat_baseline"]),
    )
    if irq_lost:
        print(f"  WARNING: {irq_lost}/{IRQ_SAMPLES} IRQ pings lost (SGI not delivered)")
    if con_lost:
        print(f"  WARNING: {con_lost}/{MUTEX_SAMPLES} contended acquires hit the retry cap")

    total = switch_count + resume_count
    p = switch_count / total if total else 0.0
    print("\nmicrobenchmarks (cycles):")
    for name, v in (("ctxsw switch", res.ctxsw_switch), ("ctxsw resume", res.ctxsw_resume),
                    ("irq dispatch", res.irqlat), ("mutex uncont", res.mutex_unc),
                    ("mutex cont",   res.mutex_con),
                    ("memlat sdram rd", res.memlat_sdram_rd), ("memlat sdram wr", res.memlat_sdram_wr),
                    ("memlat ocram rd", res.memlat_ocram_rd), ("memlat ocram wr", res.memlat_ocram_wr)):
        s = dist_stats(v)
        print(f"  {name:16s} n={s['count']:5d}  min={s['min']:6d}  mean={s['mean']:8.1f}  "
              f"p99={s['p99']:6d}  max={s['max']:6d}")
    print(f"  switch fraction p = {p:.3f}  ({switch_count} switch / {resume_count} resume)")
    print(f"  memlat baseline (pmu+dsb overhead) = {res.memlat_baseline} cycles")

    return res


@dataclass(frozen=True)
class SuspendResult:
    # scenario A: a task suspends itself (forced context switch + resume)
    self_requested: int            # delay ticks asked for
    self_measured:  int            # ticks actually elapsed across the suspend call
    self_resumed:   bool           # the task returned from suspend_thread at all
    self_regs_ok:   bool           # callee-saved sentinels survived the resume
    self_ticks:     tuple[int, ...]   # tick of each suspender activation
    # scenario B: a controller suspends a different (non-running) victim
    other_requested:    int        # delay ticks asked for
    other_suspend_tick: int        # tick the suspend was issued
    other_last_pre:     int        # last victim run before the suspend
    other_first_post:   int        # first victim run after the suspend
    other_ticks:        tuple[int, ...]   # tick of each victim activation

    @property
    def other_gap(self) -> int:
        return self.other_first_post - self.other_suspend_tick


def suspend_test(ocd: OCD, symbols: dict[str, int]) -> SuspendResult:
    bp_done = symbols["ktrace_bp_suspend_done"]
    bp_fault = symbols["fault_trap"]

    # Firmware is parked on bp_balance_done (micro is swapped out). The edf/balance
    # breakpoints are all behind us now -- free them (including the one we're halted
    # on) so we stay under the Cortex-A9 6-HW-breakpoint cap, then arm suspend-done.
    for name in ("ktrace_bp_edf_ready", "ktrace_bp_edf_done",
                 "ktrace_bp_balance_ready", "ktrace_bp_balance_done"):
        ocd.remove_hw_breakpoint(symbols[name])
    ocd.add_hw_breakpoint(bp_done)

    # Both scenarios are self-terminating (budget-capped); poll rather than one
    # long wait_halt, and read g_susp_phase so a stall names the failing scenario.
    ocd.continue_from_breakpoint()

    deadline = time.time() + 60.0
    while True:
        time.sleep(1.5)
        ocd.halt()
        pc = ocd.pc()
        if pc == bp_done:
            break
        if pc == bp_fault:
            raise RuntimeError(ocd.diagnose())
        phase = ocd.read_u32(symbols["g_susp_phase"])
        if time.time() > deadline:
            raise RuntimeError(f"suspend stalled in phase {phase} "
                               f"(1=self 2=other 3=done), pc=0x{pc:08x}:\n{ocd.diagnose()}")
        print(f"  suspend running: phase={phase} pc=0x{pc:08x}")
        ocd.resume()

    def ticks(sym: str, n_sym: str) -> tuple[int, ...]:
        n = min(ocd.read_u32(symbols[n_sym]), SUSP_TICKS)
        return tuple(ocd.read_words(symbols[sym], n)) if n else ()

    res = SuspendResult(
        self_requested=ocd.read_u32(symbols["g_susp_self_requested_delay"]),
        self_measured =ocd.read_u32(symbols["g_susp_self_delay"]),
        self_resumed  =bool(ocd.read_u32(symbols["g_susp_self_resumed"])),
        self_regs_ok  =bool(ocd.read_u32(symbols["g_susp_self_regs_ok"])),
        self_ticks    =ticks("g_susp_self_ticks", "g_susp_self_n"),
        other_requested   =ocd.read_u32(symbols["g_susp_other_requested_delay"]),
        other_suspend_tick=ocd.read_u32(symbols["g_susp_other_suspend_tick"]),
        other_last_pre    =ocd.read_u32(symbols["g_susp_other_last_pre"]),
        other_first_post  =ocd.read_u32(symbols["g_susp_other_first_post"]),
        other_ticks       =ticks("g_susp_other_ticks", "g_susp_other_n"),
    )

    print("\nsuspend_thread:")
    print(f"  self  : requested={res.self_requested} measured={res.self_measured} "
          f"resumed={res.self_resumed} regs_ok={res.self_regs_ok}")
    print(f"  other : requested={res.other_requested} suspend@{res.other_suspend_tick} "
          f"last_pre={res.other_last_pre} first_post={res.other_first_post} gap={res.other_gap}")

    # --- assertions: this is a pass/fail test, not just a measurement ---
    errs: list[str] = []
    if not res.self_resumed:
        errs.append("self-suspend: task never returned from suspend_thread (forced switch/resume broken)")
    if not res.self_regs_ok:
        errs.append("self-suspend: callee-saved registers corrupted across resume")
    if not (res.self_requested <= res.self_measured <= res.self_requested + SUSP_SLACK):
        errs.append(f"self-suspend: measured delay {res.self_measured} not in "
                    f"[{res.self_requested}, {res.self_requested + SUSP_SLACK}]")
    if res.other_first_post == 0:
        errs.append("suspend-another: victim never ran again after being suspended")
    elif not (res.other_requested <= res.other_gap <= res.other_requested + SUSP_SLACK):
        errs.append(f"suspend-another: victim gap {res.other_gap} not in "
                    f"[{res.other_requested}, {res.other_requested + SUSP_SLACK}]")
    if res.other_last_pre and res.other_suspend_tick and res.other_last_pre > res.other_suspend_tick:
        errs.append("suspend-another: victim ran after the suspend tick but before the delay elapsed")
    if errs:
        raise RuntimeError("suspend_thread test FAILED:\n  " + "\n  ".join(errs))

    return res


def _trace_counts(trace: bytes) -> dict[int, int]:
    # run-ticks per id in a core's schedule trace (how many ticks each id ran).
    counts: dict[int, int] = {}
    for b in trace:
        counts[b] = counts.get(b, 0) + 1
    return counts


@dataclass(frozen=True)
class PersistResult:
    periodic_assign:  tuple[int, ...]   # periodic id -> core the balancer chose
    periodic_periods: tuple[int, ...]   # periodic id -> period (ticks)
    periodic_done:    tuple[int, ...]   # periodic id -> completions in the window
    persist_ids:      tuple[int, ...]   # persistent trace ids (HIGH..LOW)
    persist_prio:     tuple[int, ...]   # persistent id -> priority level (2=HIGH,1=MED,0=LOW)
    persist_assign:   tuple[int, ...]   # persistent id -> core the balancer chose
    core_u:           tuple[float, ...] # per-core empirical utilization
    misses:           tuple[int, ...]   # per-core missed deadlines
    traces:           tuple[bytes, ...] # per-core schedule trace (ids; 0xFF = idle)


def persistent_test(ocd: OCD, symbols: dict[str, int], num_cpus: int) -> PersistResult:
    bp_ready = symbols["ktrace_bp_persist_ready"]
    bp_done  = symbols["ktrace_bp_persist_done"]
    bp_fault = symbols["fault_trap"]

    # Firmware is parked on bp_suspend_done. Free the breakpoints behind us (the
    # one we're on, and micro's) to stay under the Cortex-A9 6-HW cap, then arm
    # persist ready/done.
    for name in ("ktrace_bp_suspend_done", "ktrace_bp_micro_done"):
        if symbols.get(name):
            ocd.remove_hw_breakpoint(symbols[name])
    ocd.add_hw_breakpoint(bp_ready)
    ocd.add_hw_breakpoint(bp_done)

    # Step into persistent_run: it builds the periodic set (auto-balanced) and the
    # persistent set, records each thread's assigned core, then stops at bp_ready
    # with interrupts still off -- nothing has run, so the assignment is final.
    ocd.continue_from_breakpoint()
    ocd.expect_breakpoint(bp_ready, timeout=20.0)

    periodic_assign  = tuple(ocd.read_words(symbols["g_pp_periodic_assign"], PP_NPERIODIC))
    periodic_periods = tuple(ocd.read_words(symbols["g_pp_periods"], PP_NPERIODIC))
    persist_ids      = tuple(ocd.read_words(symbols["g_pp_persist_ids"], PP_NPERSIST))
    persist_prio     = tuple(ocd.read_words(symbols["g_pp_persist_prio"], PP_NPERSIST))
    persist_assign   = tuple(ocd.read_words(symbols["g_pp_persist_assign"], PP_NPERSIST))

    # Run both cores for the measurement window.
    ocd.continue_from_breakpoint()
    time.sleep(PP_SECONDS)
    ocd.halt()

    if ocd.read_fault():
        raise RuntimeError(ocd.diagnose())

    core_u        = tuple(v / 65536.0 for v in ocd.read_words(symbols["g_core_u_m"], num_cpus))
    misses        = tuple(ocd.read_words(symbols["g_misses_m"], num_cpus))
    periodic_done = tuple(ocd.read_words(symbols["g_pp_periodic_done"], PP_NPERIODIC))

    traces: list[bytes] = []
    for c in range(num_cpus):
        n = min(ocd.read_u32(symbols["g_trace_len"] + 4 * c), TRACE_TICKS)
        traces.append(ocd.read_bytes(symbols["g_sched_trace"] + TRACE_TICKS * c, n) if n else b"")

    # Release the window; firmware kills its threads, tears down, stops at done.
    ocd.write_u32(symbols["g_test_release"], 1)
    ocd.resume()
    ocd.expect_breakpoint(bp_done, timeout=30.0)

    res = PersistResult(periodic_assign, periodic_periods, periodic_done,
                        persist_ids, persist_prio, persist_assign, core_u, misses, tuple(traces))

    # --- report -------------------------------------------------------------
    counts = [_trace_counts(t) for t in traces]
    level  = {2: "HIGH", 1: "MED", 0: "LOW"}
    percore_counts = [sum(1 for i in range(PP_NPERSIST) if persist_assign[i] == c) for c in range(num_cpus)]

    def run_ticks_of(i: int) -> int:
        c = persist_assign[i]
        return counts[c].get(persist_ids[i], 0) if c < len(counts) else 0

    print("\npersistent threads:")
    print("  periodic (hard EDF, auto-balanced):")
    for i in range(PP_NPERIODIC):
        print(f"    id={i} (T={periodic_periods[i]}) -> CPU{periodic_assign[i]}  done={periodic_done[i]}")
    print("  " + "  ".join(f"U(CPU{c})={core_u[c]:.3f} misses={misses[c]}" for c in range(num_cpus)))
    print("  persistent (round-robin best-effort):")
    for i in range(PP_NPERSIST):
        print(f"    id={persist_ids[i]} {level.get(persist_prio[i], '?'):4s} -> CPU{persist_assign[i]}  run_ticks={run_ticks_of(i)}")
    for c in range(num_cpus):
        total = sum(counts[c].values()) or 1
        idle  = counts[c].get(PP_IDLE_ID, 0)
        print(f"  CPU{c} idle = {idle}/{total} ticks ({100.0*idle/total:.1f}%)")
    # least_loaded_core alternates on ties, so un-started (u=0) persistent threads
    # SPREAD across cores rather than piling -- report the actual split.
    print(f"  balancer spread persistent across cores: {percore_counts} (per CPU)")

    # --- checks (non-fatal: always emit figures; print warnings on anomalies) --
    warns: list[str] = []

    # 1. periodic always wins: no missed deadlines, every periodic task ran.
    for c in range(num_cpus):
        if misses[c] != 0:
            warns.append(f"periodic: CPU{c} missed {misses[c]} deadlines (persistent stole hard-task time)")
    for i in range(PP_NPERIODIC):
        if periodic_done[i] == 0:
            warns.append(f"periodic: id={i} never completed a job in the window")

    # 2. strict priority is PER CORE: a persistent thread may run only if no higher-
    #    priority persistent thread shares its core.
    for i in range(PP_NPERSIST):
        c, prio = persist_assign[i], persist_prio[i]
        higher_here = any(persist_assign[j] == c and persist_prio[j] > prio for j in range(PP_NPERSIST))
        if higher_here and run_ticks_of(i) != 0:
            warns.append(f"strict priority: id={persist_ids[i]} ({level.get(prio,'?')}) ran "
                         f"{run_ticks_of(i)} ticks on CPU{c} under a higher-priority thread (should be 0)")

    # 3. round-robin fairness is PER (core, level): several same-priority threads
    #    sharing one core should split that core's slack evenly. The global HIGH
    #    share need NOT be even -- the balancer may split a level unevenly across
    #    cores (e.g. 2/1), and a level's lone thread on a core gets all its slack.
    groups: dict[tuple[int, int], list[int]] = {}
    for i in range(PP_NPERSIST):
        groups.setdefault((persist_assign[i], persist_prio[i]), []).append(i)
    for (c, prio), idxs in groups.items():
        if len(idxs) > 1:
            rts = [run_ticks_of(i) for i in idxs]
            if any(r == 0 for r in rts):
                warns.append(f"round-robin: starved thread in CPU{c} {level.get(prio,'?')} level (run_ticks={rts})")
            elif max(rts) - min(rts) > 2 * PP_SLICE:
                warns.append(f"round-robin: uneven share in CPU{c} {level.get(prio,'?')} level, "
                             f"spread={max(rts)-min(rts)} > {2*PP_SLICE} (run_ticks={rts})")

    # 4. fill: a core hosting any persistent thread should soak nearly all its slack.
    for c in range(num_cpus):
        has_persist = any(persist_assign[i] == c for i in range(PP_NPERSIST))
        idle_frac   = counts[c].get(PP_IDLE_ID, 0) / (sum(counts[c].values()) or 1)
        if has_persist and idle_frac > 0.02:
            warns.append(f"fill: CPU{c} hosts persistent threads but idled {idle_frac:.2f} of the window")

    if warns:
        print("  WARNINGS (test still passes; see figures):")
        for w in warns:
            print("    " + w)
    else:
        print("  all checks passed")

    return res


def plot_persistent(res: PersistResult, path: Path, num_cpus: int,
                    window: int = TRACE_TICKS) -> None:
    # One Gantt per core: periodic rows (balanced to this core) on top, then the
    # persistent rows the balancer put here, labelled by priority level. Shows
    # periodic preempting, persistent round-robin in the slack, and MED/LOW starved.
    colors = {2: "#1f77b4", 1: "#ff7f0e", 0: "#2ca02c"}   # HIGH / MED / LOW
    pcolor = "#7f7f7f"
    level  = {2: "HIGH", 1: "MED", 0: "LOW"}
    fig, axes = plt.subplots(num_cpus, 1, figsize=(16, 3.4 * num_cpus), squeeze=False)

    for c in range(num_cpus):
        ax = axes[c][0]
        periodic_here   = [i for i in range(PP_NPERIODIC) if res.periodic_assign[i] == c]
        persist_here    = [i for i in range(PP_NPERSIST) if res.persist_assign[i] == c]
        rows: list[tuple[str, int, str]] = []   # (label, trace-id, color)
        for i in periodic_here:
            rows.append((f"periodic id={i} (T={res.periodic_periods[i]})", i, pcolor))
        for i in persist_here:
            tid = res.persist_ids[i]
            rows.append((f"{level.get(res.persist_prio[i], '?')} id={tid}", tid,
                         colors.get(res.persist_prio[i], "#000000")))

        trace = res.traces[c] if c < len(res.traces) else b""
        n = min(len(trace), window)
        nr = len(rows)
        for row, (_, tid, color) in enumerate(rows):
            y = nr - 1 - row
            segs, t = [], 0
            while t < n:
                if trace[t] == tid:
                    s = t
                    while t < n and trace[t] == tid:
                        t += 1
                    segs.append((s, t - s))
                else:
                    t += 1
            ax.broken_barh(segs, (y + 0.15, 0.7), facecolors=color)
        ax.set_yticks([nr - 1 - row + 0.5 for row in range(nr)])
        ax.set_yticklabels([lbl for lbl, _, _ in rows], fontsize=8)
        ax.set_ylim(0, max(nr, 1))
        ax.set_xlim(0, max(n, 1))
        ax.set_xlabel("time (ticks)")
        idle = _trace_counts(trace).get(PP_IDLE_ID, 0)
        ax.set_title(f"CPU{c}  (U={res.core_u[c]:.3f}, misses={res.misses[c]}, "
                     f"idle={100.0*idle/(n or 1):.1f}%; first {n} ticks)")
        ax.grid(axis="x", alpha=0.3)
    fig.suptitle("Persistent threads under tight EDF: periodic preempts, persistent round-robins the slack",
                 fontsize=12)
    fig.tight_layout()
    fig.savefig(path, dpi=130)
    plt.close(fig)


def write_persistent_csv(path: Path, res: PersistResult, num_cpus: int) -> None:
    counts = [_trace_counts(t) for t in res.traces]
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kind", "id", "priority", "period", "core", "run_ticks", "done"])
        for i in range(PP_NPERIODIC):
            w.writerow(["periodic", i, "", res.periodic_periods[i], res.periodic_assign[i],
                        counts[res.periodic_assign[i]].get(i, 0), res.periodic_done[i]])
        level = {2: "HIGH", 1: "MED", 0: "LOW"}
        for i in range(PP_NPERSIST):
            tid = res.persist_ids[i]
            w.writerow(["persistent", tid, level.get(res.persist_prio[i], "?"), PP_SLICE,
                        res.persist_assign[i], counts[res.persist_assign[i]].get(tid, 0), ""])
        w.writerow([])
        for c in range(num_cpus):
            total = sum(counts[c].values()) or 1
            w.writerow([f"CPU{c}_util", round(res.core_u[c], 4), f"misses={res.misses[c]}",
                        f"idle_pct={round(100.0*counts[c].get(PP_IDLE_ID,0)/total,1)}"])


SCHED_SAMPLES = 512   # per-level capture depth (must match workload_schedsweep.c)


def schedsweep_test(ocd: OCD, symbols: dict[str, int]) -> list[tuple[int, list[int]]]:
    bp_done  = symbols["ktrace_bp_sched_sweep_done"]
    bp_fault = symbols["fault_trap"]

    # Firmware is parked on bp_persist_done. Free the breakpoints behind us (A9 6-cap)
    # and arm the schedsweep-done bp.
    for name in ("ktrace_bp_persist_ready", "ktrace_bp_persist_done"):
        if symbols.get(name):
            ocd.remove_hw_breakpoint(symbols[name])
    ocd.add_hw_breakpoint(bp_done)

    # The sweep runs 7 levels x 512 ticks autonomously (~a few seconds). Poll rather
    # than one long wait_halt.
    ocd.continue_from_breakpoint()
    deadline = time.time() + 90.0
    while True:
        time.sleep(2.0)
        ocd.halt()
        pc = ocd.pc()
        if pc == bp_done:
            break
        if pc == bp_fault:
            raise RuntimeError(ocd.diagnose())
        if time.time() > deadline:
            raise RuntimeError(f"schedsweep stalled, pc=0x{pc:08x}:\n{ocd.diagnose()}")
        ocd.resume()

    n      = min(ocd.read_u32(symbols["g_schedsweep_n"]), 32)
    levels = ocd.read_words(symbols["g_schedsweep_levels"], n)
    counts = ocd.read_words(symbols["g_schedsweep_counts"], n)
    base   = symbols["g_schedsweep_samples"]

    rows: list[tuple[int, list[int]]] = []
    for i in range(n):
        c = min(counts[i], SCHED_SAMPLES)
        samples = ocd.read_words(base + i * SCHED_SAMPLES * 4, c) if c else []
        rows.append((levels[i], samples))

    print("\nscheduler overhead vs loaded tasks (per-tick next_thread, cycles):")
    for N, samples in rows:
        s = dist_stats(samples)
        print(f"  N={N:<3} n={int(s['count']):<4} min={int(s['min']):<5} mean={s['mean']:<7.1f} "
              f"median={int(s['median']):<5} p99={int(s['p99']):<5} max={int(s['max'])}")
    return rows


def write_schedsweep_csv(path: Path, rows: list[tuple[int, list[int]]]) -> None:
    # plot_latency_ranges.py input format: one distribution row per task count.
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["distribution", "count", "min", "mean", "median", "p99", "max", "unit"])
        for N, samples in rows:
            s = dist_stats(samples)
            w.writerow([f"{N} tasks", int(s["count"]), int(s["min"]), round(s["mean"], 1),
                        int(s["median"]), int(s["p99"]), int(s["max"]), "cycles"])


def write_artifacts(outdir: Path, alloc_metrics: list[Metric], edf_results: list[EDFResult],
                    traces: dict[int, bytes], periods: tuple[int, ...],
                    cpu1_traces: dict[int, bytes] | None = None,
                    warmup: dict[str, dict[str, list[int]]] | None = None) -> None:
    # A skipped phase leaves its list empty and is simply not written.
    if alloc_metrics:
        write_alloc_csv(outdir / "alloc.csv", alloc_metrics[:4])
        write_cache_csv(outdir / "cache_workloads.csv", alloc_metrics)
        plot_alloc(alloc_metrics[:4], outdir / "alloc_timing.png")
        plot_cache(alloc_metrics, outdir / "cache_workloads.png")
    if warmup:
        # Per-iteration cost vs iteration index -- the cache/allocator warm-up curves.
        if warmup.get("alloc"):
            write_samples_csv(outdir / "alloc_samples.csv", warmup["alloc"])
            plot_warmup(warmup["alloc"], outdir / "alloc_warmup.png",
                        "Allocator op cost per iteration")
        if warmup.get("rmw"):
            write_samples_csv(outdir / "rmw_samples.csv", warmup["rmw"])
            plot_warmup(warmup["rmw"], outdir / "rmw_warmup.png",
                        "SDRAM RMW cost per pass (cache warm-up)", xlabel="pass")
        if warmup.get("matmul"):
            write_samples_csv(outdir / "matmul_samples.csv", warmup["matmul"])
            plot_warmup(warmup["matmul"], outdir / "matmul_warmup.png",
                        "matmul cost per rep (I-cache warm-up)", xlabel="rep")

    if not edf_results:
        return
    write_edf_csv(outdir / "edf_sweep.csv", edf_results)
    plot_edf(edf_results, outdir / "edf_sweep.png")

    by_u       = {r.u_permille: r for r in edf_results}
    swept      = sorted(by_u)
    no_miss    = [u for u in swept if by_u[u].misses == 0]
    miss       = [u for u in swept if by_u[u].misses > 0]
    labels: dict[int, list[str]] = {}
    if COMFORTABLE_U in by_u: labels.setdefault(COMFORTABLE_U, []).append("comfortable")
    if no_miss:               labels.setdefault(max(no_miss), []).append("last no-miss")
    if miss:                  labels.setdefault(min(miss), []).append("first miss")
    if swept:                 labels.setdefault(max(swept), []).append("max U")

    for u in sorted(labels):
        trace = traces.get(u)
        if trace:
            plot_gantt(trace, periods, outdir / f"edf_schedule_u{u / 1000:.3f}.png", u,
                       u_configured=by_u[u].configured_u, task_u=by_u[u].measured_u_per_task,
                       label=" / ".join(labels[u]))

        # CPU1 ran the same trial concurrently; plot its Gantt from the same trial's
        # capture. Per-core CPU1 metrics aren't mirrored -> no task-U labels.
        cpu1 = (cpu1_traces or {}).get(u)
        if cpu1:
            plot_gantt(cpu1, periods, outdir / f"edf_schedule_cpu1_u{u / 1000:.3f}.png", u,
                       u_configured=by_u[u].configured_u, label="CPU1")

    clean = [r for r in edf_results if r.misses == 0]
    if clean:
        knee = max(clean, key=lambda r: r.configured_u)
        print(f"\nU* = {knee.configured_u:.3f} (requested {knee.requested_u:.3f}, measured {knee.measured_u:.3f})")


# --- main -------------------------------------------------------------------
def main(bootable: bool = False) -> None:
    if not TEST_ELF.exists():
        raise RuntimeError(f"{TEST_ELF} does not exist")

    symbols  = elf_symbols(TEST_ELF)
    num_cpus = 2 if symbols.get("_cfg_enable_smp") else 1


    bp_fault = symbols.get("fault_trap")

    balance: BalanceResult | None = None
    micro: MicroResult | None = None
    suspend: SuspendResult | None = None
    persist: PersistResult | None = None
    sched_rows = None

    outdir = RESULTS_DIR / datetime.now().strftime("%Y%m%d-%H%M%S")
    outdir.mkdir(parents=True, exist_ok=True)
    print(f"artifacts -> {outdir}\n")

    alloc_metrics: list[Metric] = []
    warmup: dict[str, dict[str, list[int]]] = {}
    edf_results: list[EDFResult] = []
    traces: dict[int, bytes] = {}
    cpu1_traces: dict[int, bytes] = {}

    with OCD() as ocd:
        if bootable:
            # Board self-booted from SD, spinning at the BOOT_TEST gate: attach, don't load.
            ocd.halt()
            print(f"bootable: attached to self-booted target, PC=0x{ocd.pc():08x}\n")
        else:
            ocd.load_image(TEST_ELF, symbols["_reset_handler"])

        ocd.fault_addr   = symbols["g_fault"]
        ocd.error_addr   = symbols["g_error_message"]
        ocd.ctx_addr     = symbols["g_error_ctx"]
        ocd.edf_bad_addr = symbols.get("g_edf_bad_thread")
        ocd.num_cpus     = num_cpus

        # A test's <name>_run symbol survives --gc-sections only if main.c calls it, so the
        # ELF is the single source of truth for which tests run -- no hand-kept control panel.
        # Each phase is gated on its marker below; EDF/balance also read config + arm their
        # breakpoints here, only when present.
        has = lambda name: name in symbols
        print("tests detected: " + ", ".join(t[:-4] for t in (
            "allocbench_run", "edf_run", "edf_balance_run", "micro_run",
            "suspend_run", "persistent_run", "schedsweep_run") if has(t)) + "\n")

        periods, u_values = (), ()
        if has("edf_run"):
            u_count = ocd.read_u32(symbols["g_edf_u_count"])
            if not 1 <= u_count <= 64:
                raise RuntimeError(f"invalid EDF U count: {u_count}")
            periods  = tuple(ocd.read_words(symbols["g_edf_periods"], EDF_TASKS))
            u_values = tuple(ocd.read_words(symbols["g_edf_u_values"], u_count))
            print(f"EDF U sweep: {list(u_values)}")
            print(f"EDF periods: {list(periods)}\n")

        # Arm all breakpoints BEFORE releasing the boot gate.
        if bp_fault:
            ocd.add_hw_breakpoint(bp_fault)
        if has("edf_run"):
            ocd.add_hw_breakpoint(symbols["ktrace_bp_edf_ready"])
            ocd.add_hw_breakpoint(symbols["ktrace_bp_edf_done"])
        if has("edf_balance_run"):
            ocd.add_hw_breakpoint(symbols["ktrace_bp_balance_ready"])
            ocd.add_hw_breakpoint(symbols["ktrace_bp_balance_done"])
        if bootable:
            ocd.write_u32(symbols["g_boot_release"], 1)

        alloc_sweeps: dict = {}
        try:
            if has("allocbench_run"):
                alloc_metrics, warmup, alloc_sweeps = allocbench(ocd, symbols)
            if has("edf_run"):
                edf_results, traces, cpu1_traces = edf_test(ocd, symbols, num_cpus, periods, u_values)
            if has("edf_balance_run"):
                balance = balance_test(ocd, symbols, num_cpus)
            if has("micro_run"):
                micro = micro_test(ocd, symbols)
            if has("suspend_run"):
                suspend = suspend_test(ocd, symbols)
            if has("persistent_run"):
                persist = persistent_test(ocd, symbols, num_cpus)
            if has("schedsweep_run"):
                sched_rows = schedsweep_test(ocd, symbols)
        except KeyboardInterrupt:
            # Ctrl-C usually means the run hung -- dump where both cores are stuck
            # (+ any fault record) before the socket closes. fault_addr/num_cpus are
            # already set above, so diagnose() gives the full picture.
            print("\n\n*** interrupted -- target state ***")
            try:
                ocd.drain()
                print(ocd.diagnose())
            except Exception as e:
                print(f"(could not read target: {e})")
            raise SystemExit(130)

    write_artifacts(outdir, alloc_metrics, edf_results, traces, periods, cpu1_traces, warmup)
    for key, sweep in alloc_sweeps.items():
        _ns, _xs, _as, _fs, xname, title_suf, xlabel, stem = ALLOC_SWEEPS[key]
        write_sweep_csv(outdir / f"{stem}.csv", sweep, xname)
        plot_sweep(sweep, outdir / f"{stem}.png", f"Allocator cost {title_suf}: Kairos TLSF", xlabel)
    if balance is not None:
        write_balance_csv(outdir / "balance.csv", balance)
        plot_balance(balance, outdir / "edf_balance.png", num_cpus)
    if micro is not None:
        total = micro.switch_count + micro.resume_count
        p = micro.switch_count / total if total else 0.0
        _write_micro_csv(outdir / "ctxsw.csv",
                         [("switch", micro.ctxsw_switch), ("resume", micro.ctxsw_resume)],
                         extra=[[], ["switch_fraction_p", round(p, 4)],
                                ["switch_count", micro.switch_count], ["resume_count", micro.resume_count]])
        _write_micro_csv(outdir / "irqlat.csv", [("irq_dispatch", micro.irqlat)])
        _write_micro_csv(outdir / "mutex.csv",
                         [("uncontended", micro.mutex_unc), ("contended", micro.mutex_con)])
        _write_micro_csv(outdir / "memlat.csv",
                         [("sdram_read", micro.memlat_sdram_rd), ("sdram_write", micro.memlat_sdram_wr),
                          ("ocram_read", micro.memlat_ocram_rd), ("ocram_write", micro.memlat_ocram_wr)],
                         extra=[["baseline_cycles", micro.memlat_baseline]])
        plot_ctxsw(micro, outdir / "ctxsw.png")
        plot_irqlat(micro, outdir / "irqlat.png")
        plot_mutex(micro, outdir / "mutex.png")
        plot_memlat(micro, outdir / "memlat.png")
    if suspend is not None:
        _write_micro_csv(outdir / "suspend.csv",
                         [("self_run_ticks", suspend.self_ticks), ("victim_run_ticks", suspend.other_ticks)],
                         extra=[["self_requested", suspend.self_requested],
                                ["self_measured", suspend.self_measured],
                                ["self_resumed", int(suspend.self_resumed)],
                                ["self_regs_ok", int(suspend.self_regs_ok)],
                                ["other_requested", suspend.other_requested],
                                ["other_suspend_tick", suspend.other_suspend_tick],
                                ["other_first_post", suspend.other_first_post],
                                ["other_gap", suspend.other_gap]])
        plot_suspend(suspend, outdir / "suspend.png")
    if persist is not None:
        write_persistent_csv(outdir / "persistent.csv", persist, num_cpus)
        plot_persistent(persist, outdir / "persistent.png", num_cpus)
    if sched_rows:
        write_schedsweep_csv(outdir / "schedsweep.csv", sched_rows)
        try:
            subprocess.run([sys.executable, str(ROOT / "scripts" / "plot_latency_ranges.py"),
                            str(outdir / "schedsweep.csv"), str(outdir / "schedsweep.png"),
                            "Scheduler overhead vs loaded task count (per-tick next_thread, cycles)"],
                           check=True)
        except Exception as e:
            print(f"(schedsweep plot skipped: {e})")
    print(f"\nPASS   artifacts in {outdir}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="DE1-SoC RTOS test driver")
    parser.add_argument("--bootable", "--live", action="store_true", dest="bootable",
                        help="attach to a self-booted target spinning at the BOOT_TEST gate "
                             "and release it, instead of load_image-ing the ELF over JTAG.")
    args = parser.parse_args()
    main(bootable=args.bootable)
