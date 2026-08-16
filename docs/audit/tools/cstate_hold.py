#!/usr/bin/env python3
"""
Hold a zero CPU-latency request on /dev/cpu_dma_latency for N seconds.

The kernel honours a PM-QoS latency request only while the file descriptor stays open, so
this has to be a live process for the duration of the measurement -- writing and closing does
nothing.

Why it matters here: docs/audit/08 section 4 measured the same engine code running 5.6x
slower immediately after a real sleep (frame execution p50 16.4us vs 2.9us). That is the core
going cold -- C-state exit latency and frequency ramp -- and the reason -pingboost 4 needed a
busy-spin guard window to match -pingboost 3's warm-core execution. Capping C-state exit
latency addresses the same cause without burning a core in userspace, so if it works it is
strictly better than spinning.

Needs write access to /dev/cpu_dma_latency (root, or chmod 666).

Usage:  ./cstate_hold.py <seconds>
"""

import struct
import sys
import time

DEV = "/dev/cpu_dma_latency"


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: cstate_hold.py <seconds>")
    secs = float(sys.argv[1])

    try:
        fd = open(DEV, "wb")
    except PermissionError:
        sys.exit("error: cannot open %s for writing (need root or chmod 666)" % DEV)
    except FileNotFoundError:
        sys.exit("error: %s does not exist (no cpuidle driver?)" % DEV)

    # 0 microseconds of tolerable latency = keep CPUs in the shallowest idle state.
    fd.write(struct.pack("i", 0))
    fd.flush()
    print("cpu_dma_latency held at 0 us for %.0f s" % secs, flush=True)

    try:
        time.sleep(secs)
    finally:
        fd.close()   # releasing the fd drops the constraint


if __name__ == "__main__":
    main()
