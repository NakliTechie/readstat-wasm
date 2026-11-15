#!/bin/bash
# Rebuild readstat.wasm and readstat.mjs from ReadStat at the pinned commit.
# Needs Emscripten (emcc) on PATH. Output replaces the committed files.
set -euo pipefail
PIN=3c68974fbb35c5bf0888fd603cd99b8253477359
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$HERE/.build"
if [ ! -d "$WORK/ReadStat" ]; then
  mkdir -p "$WORK"
  git clone -q https://github.com/WizardMac/ReadStat.git "$WORK/ReadStat"
fi
git -C "$WORK/ReadStat" checkout -q "$PIN"
SRC="$WORK/ReadStat/src"

# ReadStat's read side only: no *_write.c and no readstat_writer.c.
CORE="$SRC/CKHashTable.c $SRC/readstat_bits.c $SRC/readstat_convert.c \
  $SRC/readstat_error.c $SRC/readstat_io_unistd.c $SRC/readstat_malloc.c \
  $SRC/readstat_metadata.c $SRC/readstat_parser.c $SRC/readstat_value.c \
  $SRC/readstat_variable.c"
FMT="$(find "$SRC/sas" "$SRC/spss" "$SRC/stata" -name '*.c' ! -name '*_write.c' | sort | tr '\n' ' ')"

emcc -O3 -I"$SRC" $CORE $FMT "$HERE/src/rs_wrapper.c" \
  -sUSE_ZLIB=1 -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createReadStat \
  -sALLOW_MEMORY_GROWTH=1 -sFILESYSTEM=1 \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToNewUTF8,getValue,HEAPU8 \
  -sEXPORTED_FUNCTIONS=_rs_read,_rs_free,_rs_ndjson,_rs_ndjson_len,_rs_columns,_rs_meta,_rs_rowcount,_rs_varcount,_rs_errmsg,_malloc,_free \
  -sENVIRONMENT=web,worker -sSINGLE_FILE=0 \
  -o "$WORK/readstat.mjs"
cp "$WORK/readstat.mjs" "$HERE/readstat.mjs"
cp "$WORK/readstat.wasm" "$HERE/readstat.wasm"
ls -la "$HERE/readstat.mjs" "$HERE/readstat.wasm"
