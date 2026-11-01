#!/usr/bin/env bash
# Runs every libFuzzer target for a while (fuzz/README.md). Usage: fuzz/run_all.sh [seconds per target] [build dir]
# Extra libFuzzer options in FUZZ_ARGS (e.g. FUZZ_ARGS="-fork=2 -ignore_crashes=1" to keep going past a crash).
# Each target's corpus grows in fuzz-work/<name>/ (seeded from fuzz/corpus/<name>/); crashes are saved as
# fuzz-work/crashes/<name>-crash-*. Exits non-zero if any target found one.
set -u
seconds=${1:-60}
build=${2:-build-fuzz}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p fuzz-work/crashes
# halt on every UBSan report except those suppressed in fuzz/ubsan.supp
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:suppressions=$here/ubsan.supp"
status=0
for bin in "$build"/xythos-fuzz-*; do
    name=$(basename "$bin")
    name=${name#xythos-fuzz-}
    [ "$name" = replay ] && continue
    corpus=${name//-/_}
    mkdir -p "fuzz-work/$corpus"
    echo "=== $corpus ($seconds s)"
    # -rss_limit_mb: the engine target reserves the looper's 23 MB on top of ASan's overhead
    if ! "$bin" "fuzz-work/$corpus" "$here/corpus/$corpus" -max_total_time="$seconds" -rss_limit_mb=4096 \
            -max_len=65536 -timeout=30 -print_final_stats=1 -artifact_prefix="fuzz-work/crashes/$corpus-" ${FUZZ_ARGS:-} \
            > "fuzz-work/$corpus.log" 2>&1; then
        echo "FAILED: see fuzz-work/$corpus.log"
        grep -E "^(==[0-9]+==ERROR|SUMMARY|FUZZ_CHECK)|runtime error" "fuzz-work/$corpus.log" | head -5
        status=1
    fi
    grep -E "stat::number_of_executed_units|cov:" "fuzz-work/$corpus.log" | tail -2
done
exit $status
