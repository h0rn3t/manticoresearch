#!/usr/bin/env python3
# Generate a deterministic TSV dataset for the io_uring benchmark plain index.
# Columns: id <tab> gid <tab> title
# A large vocabulary + many docs yields sizable doclists/hitlists on disk, which
# is exactly what access_doclists=file / access_hitlists=file read on demand.
import itertools
import random
import sys

n_docs = int(sys.argv[1]) if len(sys.argv) > 1 else 1_500_000
vocab_size = int(sys.argv[2]) if len(sys.argv) > 2 else 40_000
words_per_doc = int(sys.argv[3]) if len(sys.argv) > 3 else 16
out = sys.argv[4] if len(sys.argv) > 4 else "/tmp/iou_data.tsv"

rng = random.Random(1234567)  # deterministic: same index for every run

# Zipf-ish vocabulary so some terms have long doclists (the interesting reads).
vocab = [f"w{i}" for i in range(vocab_size)]
# Precompute cumulative weights ONCE; passing cum_weights makes choices O(k log n)
# per call instead of recomputing the cumulative table every call.
cum_weights = list(itertools.accumulate(1.0 / (i + 1) for i in range(vocab_size)))

with open(out, "w", buffering=1 << 20) as f:
    for doc_id in range(1, n_docs + 1):
        words = rng.choices(vocab, cum_weights=cum_weights, k=words_per_doc)
        gid = rng.randint(1, 1000)
        f.write(f"{doc_id}\t{gid}\t{' '.join(words)}\n")

print(f"wrote {n_docs} docs, vocab={vocab_size}, to {out}", file=sys.stderr)
