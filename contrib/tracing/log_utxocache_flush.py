#!/usr/bin/env python3

import sys
import ctypes
from bcc import BPF, USDT

""" Example script to log details about coins flushed by the Bitcoin client
utilizing USDT probes and the flush:flush tracepoint. """

# USAGE:  ./contrib/tracing/log_utxocache_flush.py path/to/bitcoind

# BCC: The C program to be compiled to an eBPF program (by BCC) and loaded into
# a sandboxed Linux kernel VM.
program = """
# include <uapi/linux/ptrace.h>
struct data_t
{
  u64 duration;
  u32 mode;
  u64 coins_count;
  u64 coins_mem_usage;
  bool is_flush_prune;
  bool is_full_flush;
};
// BPF perf buffer to push the data to user space.
BPF_PERF_OUTPUT(flush);
int trace_flush(struct pt_regs *ctx) {
  struct data_t data = {};
  bpf_usdt_readarg(1, ctx, &data.duration);
  bpf_usdt_readarg(2, ctx, &data.mode);
  bpf_usdt_readarg(3, ctx, &data.coins_count);
  bpf_usdt_readarg(4, ctx, &data.coins_mem_usage);
  bpf_usdt_readarg(5, ctx, &data.is_flush_prune);
  bpf_usdt_readarg(5, ctx, &data.is_full_flush);
  flush.perf_submit(ctx, &data, sizeof(data));
  return 0;
}
"""

FLUSH_MODES = [
    'NONE',
    'IF_NEEDED',
    'PERIODIC',
    'ALWAYS'
]


# define output data structure
class Data(ctypes.Structure):
    _fields_ = [
        ("duration", ctypes.c_uint64),
        ("mode", ctypes.c_uint32),
        ("coins_count", ctypes.c_uint64),
        ("coins_mem_usage", ctypes.c_uint64),
        ("is_flush_prune", ctypes.c_bool),
        ("is_full_flush", ctypes.c_bool)
    ]


def print_event(event):
    print("%-15d %-10s %-15d %-15s %-8s %-8s" % (
        event.duration,
        FLUSH_MODES[event.mode],
        event.coins_count,
        "%.2f kB" % (event.coins_mem_usage/1000),
        event.is_flush_prune,
        event.is_full_flush
    ))


def main(bitcoind_path):
    bitcoind_with_usdts = USDT(path=str(bitcoind_path))

    # attaching the trace functions defined in the BPF program
    # to the tracepoints
    bitcoind_with_usdts.enable_probe(
        probe="flush", fn_name="trace_flush")
    b = BPF(text=program, usdt_contexts=[bitcoind_with_usdts])

    def handle_flush(_, data, size):
        """ Coins Flush handler.
          Called each time coin caches and indexes are flushed."""
        event = ctypes.cast(data, ctypes.POINTER(Data)).contents
        print_event(event)

    b["flush"].open_perf_buffer(handle_flush)
    print("Logging utxocache flushes. Ctrl-C to end...")
    print("%-15s %-10s %-15s %-15s %-8s %-8s" % ("Duration (µs)", "Mode",
                                                 "Coins Count", "Memory Usage",
                                                 "Prune", "Full Flush"))

    while True:
        try:
            b.perf_buffer_poll()
        except KeyboardInterrupt:
            exit(0)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("USAGE: ", sys.argv[0], "path/to/bitcoind")
        exit(1)

    path = sys.argv[1]
    main(path)
