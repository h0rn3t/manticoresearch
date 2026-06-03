#!/usr/bin/env python3
# Concurrent query load driver for the io_uring benchmark. Fires full-text
# searches at searchd's HTTP JSON API for a fixed duration across C workers,
# then prints JSON metrics (qps, latency percentiles). Stdlib only.
import json
import random
import sys
import threading
import time
import urllib.request

URL = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:9308/search"
INDEX = sys.argv[2] if len(sys.argv) > 2 else "idx"
CONCURRENCY = int(sys.argv[3]) if len(sys.argv) > 3 else 16
DURATION = float(sys.argv[4]) if len(sys.argv) > 4 else 15.0
VOCAB = int(sys.argv[5]) if len(sys.argv) > 5 else 40_000

stop_at = None
lat = []
errors = 0
lock = threading.Lock()


def worker(seed):
    global errors
    rng = random.Random(seed)
    local = []
    local_err = 0
    while time.monotonic() < stop_at:
        # 2-3 term match query over a wide vocab => varied doclist/hitlist reads.
        terms = " ".join(f"w{rng.randint(0, VOCAB - 1)}" for _ in range(rng.randint(2, 3)))
        body = json.dumps({
            "index": INDEX,
            "query": {"match": {"title": terms}},
            "limit": 20,
        }).encode()
        t0 = time.monotonic()
        try:
            req = urllib.request.Request(URL, data=body, headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=30) as r:
                r.read()
            local.append((time.monotonic() - t0) * 1000.0)
        except Exception:
            local_err += 1
    with lock:
        lat.extend(local)
        errors += local_err


def main():
    global stop_at
    # brief warm-up of connections is implicit; start measuring immediately.
    stop_at = time.monotonic() + DURATION
    threads = [threading.Thread(target=worker, args=(i + 1,)) for i in range(CONCURRENCY)]
    t_start = time.monotonic()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t_start

    n = len(lat)
    lat.sort()

    def pct(p):
        if not lat:
            return 0.0
        return lat[min(n - 1, int(p / 100.0 * n))]

    print(json.dumps({
        "queries": n,
        "errors": errors,
        "elapsed_s": round(elapsed, 3),
        "qps": round(n / elapsed, 1) if elapsed > 0 else 0,
        "p50_ms": round(pct(50), 2),
        "p95_ms": round(pct(95), 2),
        "p99_ms": round(pct(99), 2),
    }))


if __name__ == "__main__":
    main()
