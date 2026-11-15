import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import { formatFor, loadReadStat, readStat, readStatRows } from '../index.mjs';

const fixture = (name) => readFileSync(new URL(`./fixtures/${name}`, import.meta.url));

test('reads Stata .dta values, dates and display formats', async () => {
  const { rows, columns, meta } = await readStatRows(fixture('stat_demo.dta'), 'dta');
  assert.deepEqual(columns, ['city', 'pop', 'code']);
  assert.deepEqual(rows[0], { city: 'Mumbai', pop: 20.4, code: 1 });
  assert.equal(meta[0].format, '%-9s');
  const dates = await readStatRows(fixture('stat_dates.dta'), 'stat_dates.dta');
  assert.equal(dates.rows[0].d, '2020-01-01');
  assert.equal(dates.rows[0].ts, '2020-01-01 13:30:00');
});

test('decodes Stata period formats to the date each period starts', async () => {
  const { rows, meta, columns } = await readStatRows(fixture('stat_periods.dta'), 'dta');
  assert.equal(rows.length, 5);
  for (const c of ['w', 'm', 'q', 'h', 'y']) assert.equal(rows[0][c], '2020-01-01');
  assert.equal(meta[columns.indexOf('m')].format, '%tm');
});

test('reads SPSS .sav and SAS .xpt dates, datetimes and times', async () => {
  const sav = await readStatRows(fixture('spss_dates.sav'), 'sav');
  assert.deepEqual(sav.rows[0], {
    id: 1, d: '2020-01-01', adate: '2020-01-01', edate: '2020-01-01',
    ts: '2020-01-01 13:30:00', tm: '13:30:00', x: 3.5,
  });
  const xpt = await readStat(fixture('sas_dates.xpt'), 'xpt');
  assert.equal(xpt.rowCount, 3);
  assert.deepEqual(xpt.columns, ['id', 'd', 'ts', 'tm', 'x']);
});

test('reports a parse error for bytes that are not a data file', async () => {
  await assert.rejects(readStat(new TextEncoder().encode('not a file'), 'dta'), /could not read dta file/);
});

test('maps file names to formats and accepts a module from loadReadStat', async () => {
  assert.equal(formatFor('survey.ZSAV'), 'sav');
  assert.equal(formatFor('notes.txt'), null);
  const module = await loadReadStat({ wasm: fixtureWasm() });
  const t = await readStat(fixture('stat_demo.dta'), 'dta', { module });
  assert.equal(t.rowCount, 3);
});

function fixtureWasm() {
  return readFileSync(new URL('../readstat.wasm', import.meta.url));
}
