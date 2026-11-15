<h1 align="center">readstat-wasm</h1>

<p align="center"><b>Read SPSS, Stata and SAS data files in the browser or Node, with no server.<br>
ReadStat compiled to WebAssembly, 275 KB, one function call per file.</b></p>

<p align="center">Browser and Node 20+. No dependencies. MIT.</p>

<p align="center">
<img alt="npm" src="https://img.shields.io/npm/v/readstat-wasm?style=flat-square&color=555">
<img alt="licence MIT" src="https://img.shields.io/badge/licence-MIT-555?style=flat-square">
<img alt="dependencies none" src="https://img.shields.io/badge/dependencies-none-555?style=flat-square">
</p>

## Install

| Where | How |
|---|---|
| npm | `npm install readstat-wasm` |
| CDN | `import { readStatRows } from 'https://cdn.jsdelivr.net/npm/readstat-wasm@0.1.0/index.mjs'` |
| GitHub tag | `https://cdn.jsdelivr.net/gh/NakliTechie/readstat-wasm@v0.1.0/index.mjs` |

Pass the file's bytes and its format, or its file name:

```js
import { readStatRows } from 'readstat-wasm';

const bytes = new Uint8Array(await file.arrayBuffer());   // a File from <input type=file>
const { rows, columns, meta } = await readStatRows(bytes, file.name);
console.log(columns, rows[0], meta[0]);   // meta: each variable's display format and label
```

The module loads `readstat.wasm` from beside `index.mjs` the first time you call it.
Nothing leaves the page.

## Why

You have a `.sav` from a survey vendor or a `.dta` from a colleague, and the tool you
work in reads CSV. ReadStat is the C library that R's `haven` and Python's `pyreadstat`
use to read these files. This package builds ReadStat's read side to WebAssembly so a web
page can open the file directly.

| Format | Extensions | Notes |
|---|---|---|
| SPSS | `.sav`, `.zsav`, `.por` | `.zsav` through zlib |
| Stata | `.dta` | |
| SAS | `.sas7bdat`, `.xpt` | |

Limits: read only (no writers), inputs under 2 GB (32-bit WebAssembly), and value labels
are not returned yet.

## Dates

Dates come back as ISO strings, so a database or a CSV reader types them correctly:

- Stata `%td` and `%tc` become a date and a timestamp. The period formats `%tw`, `%tm`,
  `%tq`, `%th` and `%ty` become the date the period starts on, and `meta` keeps the
  original format so you can write the period back.
- SPSS `DATE`, `ADATE`, `EDATE`, `JDATE`, `SDATE`, `DATETIME` and `TIME` are decoded.
- SAS date, datetime and time formats (`DATE`, `MMDDYY`, `DATETIME`, `TIME` and others)
  are decoded.

Any other format stays a number.

## Large files

`readStat()` returns the rows as NDJSON bytes (one JSON object per line) instead of
objects, which DuckDB's `read_json_auto`, Arrow and most loaders read directly:

```js
import { readStat } from 'readstat-wasm';
const { ndjson, rowCount, columns } = await readStat(bytes, 'sav');
```

To host the WebAssembly file yourself, call `loadReadStat({ wasm: url })` once and pass
its result as `{ module }` to `readStat`.

## Commands

```sh
npm test        # read every fixture in test/fixtures and check values, dates and formats
./build.sh      # rebuild readstat.wasm from ReadStat at the pinned commit (needs emcc)
```

## Verify it yourself

```sh
npm test
```

The tests read one Stata, one SPSS and one SAS file of known contents (written by
`pyreadstat`, with a row before 1960 so a wrong epoch fails). `build.sh` pins ReadStat to
commit `3c68974f`; rebuilding with a different Emscripten version can change the bytes of
the `.wasm` file but not what it reads.

## Licence

MIT. ReadStat is Copyright (c) 2013-2016 Evan Miller, MIT (`LICENSE-ReadStat`).

[ReadStat](https://github.com/WizardMac/ReadStat) · [C wrapper](src/rs_wrapper.c) · [build script](build.sh)
