#!/usr/bin/env bash
# io_uring benchmark orchestrator. Builds one plain disk index (doclists/hitlists
# in access=file mode, the io_uring target), then measures query throughput with
# io_uring on vs off, cold vs warm page cache. Emits a markdown table.
#
# Env: SEARCHD, INDEXER (paths to binaries). Optional: NDOCS, VOCAB, CONC, DUR.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SEARCHD="${SEARCHD:?set SEARCHD to the searchd binary}"
INDEXER="${INDEXER:?set INDEXER to the indexer binary}"
NDOCS="${NDOCS:-1500000}"
VOCAB="${VOCAB:-40000}"
CONC="${CONC:-16}"
DUR="${DUR:-15}"

WORK=/tmp/iou
DATA=$WORK/data.tsv
IDXDIR=$WORK/index
CONF=$WORK/manticore.conf
rm -rf "$WORK"; mkdir -p "$IDXDIR"

echo "== generating data ($NDOCS docs) =="
python3 "$HERE/gen_data.py" "$NDOCS" "$VOCAB" 16 "$DATA"
ls -lh "$DATA"

write_conf() { # $1 = io_uring value
cat > "$CONF" <<EOF
source iou_src {
  type = tsvpipe
  tsvpipe_command = cat $DATA
  tsvpipe_attr_uint = gid
  tsvpipe_field = title
}
index iou_idx {
  type = plain
  source = iou_src
  path = $IDXDIR/idx
  access_doclists = file
  access_hitlists = file
  access_plain_attrs = mmap
  access_blob_attrs = mmap
}
searchd {
  listen = 127.0.0.1:9306:mysql41
  listen = 127.0.0.1:9308:http
  log = $WORK/searchd.log
  query_log = $WORK/query.log
  pid_file = $WORK/searchd.pid
  binlog_path =
  io_uring = $1
}
EOF
}

echo "== building index =="
write_conf 1
"$INDEXER" -c "$CONF" --all || { echo "indexer failed"; exit 1; }

wait_ready() {
  for _ in $(seq 1 60); do
    curl -s -o /dev/null "http://127.0.0.1:9308/" && return 0
    sleep 0.5
  done
  return 1
}

start_searchd() { "$SEARCHD" -c "$CONF" >/dev/null 2>&1; wait_ready || { echo "searchd not ready"; tail -50 "$WORK/searchd.log"; exit 1; }; }
# Never block forever on shutdown (a crashed/stuck daemon would otherwise hang CI):
# bounded --stopwait, then force-kill as a fallback.
stop_searchd() { timeout 30 "$SEARCHD" -c "$CONF" --stopwait >/dev/null 2>&1 || true; pkill -9 -f "$SEARCHD" >/dev/null 2>&1 || true; sleep 1; }

run_one() { # $1=io_uring $2=cache(cold|warm) -> echoes json
  local iou="$1" cache="$2"
  write_conf "$iou"
  stop_searchd
  if [ "$cache" = "cold" ]; then
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1 || true
    start_searchd
  else
    start_searchd
    python3 "$HERE/load.py" "http://127.0.0.1:9308/search" iou_idx "$CONC" 5 "$VOCAB" >/dev/null 2>&1 # warm-up
  fi
  python3 "$HERE/load.py" "http://127.0.0.1:9308/search" iou_idx "$CONC" "$DUR" "$VOCAB"
  # capture what searchd actually did with io_uring
  grep -o 'io_uring: [a-z ]*' "$WORK/searchd.log" | tail -1 >&2 || true
  stop_searchd
}

echo "== running matrix =="
declare -A R
for iou in 1 0; do
  for cache in cold warm; do
    echo "-- io_uring=$iou cache=$cache --" >&2
    R["$iou.$cache"]="$(run_one "$iou" "$cache")"
    echo "${R["$iou.$cache"]}" >&2
  done
done

# also report what searchd logged for io_uring status
STATUS="$(grep -o 'io_uring: [a-z ]*' "$WORK/searchd.log" | sort -u | tr '\n' ';')"

# server-side diagnostics for the error investigation
echo "===== searchd.log WARNING/ERROR lines =====" >&2
grep -iE 'warning|error|fatal|assert|crash|backtrace' "$WORK/searchd.log" | tail -40 >&2 || true
echo "===== query.log tail =====" >&2
tail -5 "$WORK/query.log" 2>/dev/null >&2 || true

emit() { # to stdout (and caller tees to summary)
  echo "### io_uring benchmark (ubuntu-24.04 runner, access=file doclists/hitlists)"
  echo ""
  echo "Docs: $NDOCS, vocab: $VOCAB, concurrency: $CONC, duration: ${DUR}s/run"
  echo "searchd io_uring log: \`$STATUS\`"
  echo ""
  echo "| io_uring | cache | QPS | p50 ms | p95 ms | p99 ms | queries | errors |"
  echo "|---|---|---|---|---|---|---|---|"
  for iou in 1 0; do
    for cache in cold warm; do
      j="${R["$iou.$cache"]:-{}}"
      python3 - "$iou" "$cache" "$j" <<'PY'
import json,sys
iou,cache,j=sys.argv[1],sys.argv[2],sys.argv[3]
try: d=json.loads(j)
except Exception: d={}
g=lambda k: d.get(k,"-")
print(f"| {'on' if iou=='1' else 'off'} | {cache} | {g('qps')} | {g('p50_ms')} | {g('p95_ms')} | {g('p99_ms')} | {g('queries')} | {g('errors')} |")
PY
    done
  done
  echo ""
  echo "> Runner disk is virtualized; treat absolute numbers as indicative and read the io_uring-on vs off delta, especially the cold-cache rows."
}

emit
