#!/usr/bin/env node
// Renders marketing/social.png (1280x640) for NakliTechie/readstat-wasm.
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const out = join(here, 'social.png');
const CHROME =
  process.env.CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const tmp = mkdtempSync(join(tmpdir(), 'readstat-social-'));

const html = `<!doctype html>
<html><head><meta charset="utf-8"><style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  html, body {
    width: 1280px; height: 640px; overflow: hidden;
    background: #ffffff;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Inter, sans-serif;
    color: #18181b;
  }
  .card {
    width: 1280px; height: 640px;
    padding: 72px 80px;
    display: grid;
    grid-template-columns: 620px 1fr;
    gap: 48px;
    align-items: center;
    border: 1px solid #e4e4e7;
  }
  .left {
    height: 100%;
    display: flex;
    flex-direction: column;
    justify-content: space-between;
  }
  .brand {
    display: flex;
    align-items: center;
    gap: 16px;
    font-size: 54px;
    font-weight: 800;
    letter-spacing: -0.025em;
    color: #18181b;
  }
  .bar {
    width: 14px;
    height: 44px;
    border-radius: 2px;
    background: #b5381b;
  }
  .pitch {
    font-size: 40px;
    font-weight: 700;
    line-height: 1.2;
    letter-spacing: -0.02em;
    color: #18181b;
  }
  .foot {
    font-size: 18px;
    font-weight: 700;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: #52525b;
  }
  .foot span { color: #b5381b; }
  .code {
    background: #18181b;
    color: #f4f4f5;
    border-radius: 12px;
    padding: 28px 30px;
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    font-size: 19px;
    line-height: 1.6;
    box-shadow: 0 12px 32px rgba(24, 24, 27, 0.08);
  }
  .dim { color: #a1a1aa; }
  .kw { color: #f87171; }
  .str { color: #86efac; }
  . pills {
    margin-top: 18px;
    padding-top: 16px;
    border-top: 1px solid #27272a;
    display: flex;
    gap: 10px;
    flex-wrap: wrap;
    font-size: 15px;
    color: #d4d4d8;
  }
  .pill {
    background: #27272a;
    padding: 4px 10px;
    border-radius: 6px;
  }
</style></head>
<body><div class="card">
  <div class="left">
    <div class="brand">readstat-wasm <div class="bar"></div></div>
    <div class="pitch">Read SPSS, Stata and SAS files in the browser or Node, with no server.</div>
    <div class="foot"><span>275 KB WASM</span> &nbsp;·&nbsp; ZERO DEPENDENCIES &nbsp;·&nbsp; MIT</div>
  </div>
  <div class="code">
    <div><span class="kw">import</span> { readStatRows } <span class="kw">from</span> <span class="str">'readstat-wasm'</span>;</div>
    <div style="height:10px"></div>
    <div><span class="kw">const</span> out = <span class="kw">await</span> readStatRows(bytes, <span class="str">'survey.sav'</span>);</div>
    <div class="dim">// → { rows, columns, meta }</div>
    <div class="pills" style="margin-top:18px;padding-top:16px;border-top:1px solid #27272a;display:flex;gap:10px;flex-wrap:wrap;font-size:15px;color:#d4d4d8">
      <span class="pill">.sav</span>
      <span class="pill">.zsav</span>
      <span class="pill">.por</span>
      <span class="pill">.dta</span>
      <span class="pill">.sas7bdat</span>
      <span class="pill">.xpt</span>
    </div>
  </div>
</div></body></html>`;

try {
  const plate = join(tmp, 'plate.html');
  writeFileSync(plate, html);
  execFileSync(
    CHROME,
    [
      '--headless=new',
      '--hide-scrollbars',
      `--user-data-dir=${join(tmp, 'profile')}`,
      '--window-size=1280,640',
      '--force-device-scale-factor=1',
      `--screenshot=${out}`,
      pathToFileURL(plate).href,
    ],
    { stdio: 'ignore' },
  );
  console.log(`wrote ${out}`);
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
