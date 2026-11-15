// Thin wrapper around ReadStat for the browser: parse an in-memory
// statistical-format buffer and emit NDJSON (one JSON object per row,
// keyed by variable name) plus a column-name list. The JS side hands the
// NDJSON to DuckDB's read_json_auto — same shape as the sql.js SQLite
// reader. Compiled to wasm via emcc (see build.sh).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "readstat.h"

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  int oom;  // set once a realloc fails; further appends become no-ops
} strbuf;

static void sb_ensure(strbuf *sb, size_t extra) {
  if (sb->oom) return;
  if (sb->len + extra + 1 > sb->cap) {
    size_t ncap = sb->cap ? sb->cap : 4096;
    while (ncap < sb->len + extra + 1) ncap *= 2;
    // realloc into a temp so a NULL return (wasm OOM) doesn't leak the old
    // buffer or leave sb->buf dangling — flag OOM and stop appending instead of
    // memcpy'ing into NULL.
    char *nbuf = realloc(sb->buf, ncap);
    if (!nbuf) {
      sb->oom = 1;
      return;
    }
    sb->buf = nbuf;
    sb->cap = ncap;
  }
}
static void sb_puts(strbuf *sb, const char *s) {
  size_t n = strlen(s);
  sb_ensure(sb, n);
  if (sb->oom) return;
  memcpy(sb->buf + sb->len, s, n);
  sb->len += n;
  sb->buf[sb->len] = 0;
}
static void sb_putc(strbuf *sb, char c) {
  sb_ensure(sb, 1);
  if (sb->oom) return;
  sb->buf[sb->len++] = c;
  sb->buf[sb->len] = 0;
}
// Append a JSON string literal (with surrounding quotes + escaping).
static void sb_json_str(strbuf *sb, const char *s) {
  sb_putc(sb, '"');
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
      case '"': sb_puts(sb, "\\\""); break;
      case '\\': sb_puts(sb, "\\\\"); break;
      case '\n': sb_puts(sb, "\\n"); break;
      case '\r': sb_puts(sb, "\\r"); break;
      case '\t': sb_puts(sb, "\\t"); break;
      default:
        if (c < 0x20) {
          char tmp[8];
          snprintf(tmp, sizeof tmp, "\\u%04x", c);
          sb_puts(sb, tmp);
        } else {
          sb_putc(sb, (char)c);
        }
    }
  }
  sb_putc(sb, '"');
}

// Date decoding. All three families store dates as numeric offsets from a
// format-family epoch, tagged by a per-variable display format. Without decoding
// a date column mounts as a meaningless number (21915 for 2020-01-01); we convert
// recognized formats to an ISO string so DuckDB's read_json_auto types the column
// as DATE/TIMESTAMP/TIME.
//   Stata (%td/%tc):   days / milliseconds since 1960-01-01
//   SPSS  (DATE… etc): seconds since 1582-10-14 (dates land on midnight)
//   SAS   (DATE… etc): days since 1960-01-01; DATETIME seconds since 1960-01-01
//   TIME  (SPSS+SAS):  seconds since midnight (a duration — may exceed 24h)
// Unrecognized formats stay raw numeric (README "Known limitations").
enum {
  DATE_NONE = 0,
  DATE_DAILY = 1,      // days since 1960-01-01 → DATE (Stata %td, SAS DATE…)
  DATE_DATETIME = 2,   // ms since 1960-01-01 → TIMESTAMP (Stata %tc)
  DATE_SPSS_DATE = 3,  // seconds since 1582-10-14 → DATE (SPSS DATE/ADATE/…)
  DATE_SPSS_DT = 4,    // seconds since 1582-10-14 → TIMESTAMP (SPSS DATETIME)
  DATE_SAS_DT = 5,     // seconds since 1960-01-01 → TIMESTAMP (SAS DATETIME)
  DATE_TIME_SECS = 6,  // seconds since midnight → TIME (SPSS/SAS TIME)
  // Stata period formats: decoded to the DATE the period starts on.
  // The original format travels in rs_meta() so an export can restore it.
  DATE_WEEK = 7,     // %tw: weeks since 1960w1 (52 per year; week 52 runs to Dec 31)
  DATE_MONTH = 8,    // %tm: months since 1960m1
  DATE_QUARTER = 9,  // %tq: quarters since 1960q1
  DATE_HALF = 10,    // %th: half-years since 1960h1
  DATE_YEAR = 11,    // %ty: the calendar year itself
};

// Days from 1960-01-01 (Stata/SAS epoch) to 1970-01-01 (the civil algorithm's epoch).
#define STATA_EPOCH_DAYS 3653
// Days from 1582-10-14 (SPSS epoch) to 1970-01-01 (= 12219379200 s / 86400).
#define SPSS_EPOCH_DAYS 141428

typedef struct {
  strbuf out;   // NDJSON rows
  strbuf cols;  // JSON array of column names
  char **names;
  int *datekind;  // per-variable date kind (DATE_NONE / DATE_* above)
  char **formats; // per-variable display format, for rs_meta()
  char **labels;  // per-variable label, for rs_meta()
  strbuf meta;    // JSON array of {"format","label"} per variable
  int var_count;
  int cur_obs;
  int started_row;
  int row_count;
  int fmt;  // file format code (0=dta 1=sav 2=por 3=sas7bdat 4=xport)
  int oom;  // a calloc/strdup failed — rs_read reports it as a distinct code
} rs_ctx;

static rs_ctx g;

// Classify a Stata display format string → date kind. `%td…`/`%d…` = daily,
// `%tc…`/`%tC…` = datetime (ms). Period formats (%tw/%tm/%tq/%th/%ty) decode to
// the first day of the period; the format itself is kept in rs_meta.
static int stata_date_kind(const char *fmt) {
  if (!fmt || fmt[0] != '%') return DATE_NONE;
  if (fmt[1] == 't') {
    switch (fmt[2]) {
      case 'c': case 'C': return DATE_DATETIME;
      case 'd': return DATE_DAILY;
      case 'w': return DATE_WEEK;
      case 'm': return DATE_MONTH;
      case 'q': return DATE_QUARTER;
      case 'h': return DATE_HALF;
      case 'y': return DATE_YEAR;
      default: return DATE_NONE;
    }
  }
  if (fmt[1] == 'd') return DATE_DAILY;  // legacy %d…
  return DATE_NONE;
}

// Extract the leading alphabetic token of an SPSS/SAS format string, uppercased
// ("DATETIME20" → "DATETIME", "MMDDYY10." → "MMDDYY"). Returns its length.
static size_t fmt_token(const char *fmt, char *tok, size_t cap) {
  size_t n = 0;
  for (; fmt && fmt[n] && n + 1 < cap; n++) {
    char c = fmt[n];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c < 'A' || c > 'Z') break;
    tok[n] = c;
  }
  tok[n] = 0;
  return n;
}

// SPSS print formats → date kind. Day formats (values are seconds since
// 1582-10-14, midnight-aligned): DATE, ADATE, EDATE, JDATE, SDATE. Datetimes:
// DATETIME, YMDHMS. TIME is a seconds-since-midnight duration. Period/interval
// formats (MOYR, QYR, WKYR, DTIME, MTIME…) are not a single instant → raw.
static int spss_date_kind(const char *fmt) {
  char tok[16];
  if (!fmt_token(fmt, tok, sizeof tok)) return DATE_NONE;
  if (!strcmp(tok, "DATE") || !strcmp(tok, "ADATE") || !strcmp(tok, "EDATE") ||
      !strcmp(tok, "JDATE") || !strcmp(tok, "SDATE"))
    return DATE_SPSS_DATE;
  if (!strcmp(tok, "DATETIME") || !strcmp(tok, "YMDHMS")) return DATE_SPSS_DT;
  if (!strcmp(tok, "TIME")) return DATE_TIME_SECS;
  return DATE_NONE;
}

// SAS formats → date kind. A SAS date value is days since 1960-01-01 whatever
// day-level format displays it (DATE, MMDDYY, DDMMYY, YYMMDD, JULIAN, MONYY,
// WEEKDATE, WORDDATE). DATETIME values are seconds since 1960-01-01; TIME/HHMM/
// MMSS are seconds since midnight. Anything else (incl. TOD, ISO 8601 variants)
// stays raw.
static int sas_date_kind(const char *fmt) {
  char tok[16];
  if (!fmt_token(fmt, tok, sizeof tok)) return DATE_NONE;
  if (!strcmp(tok, "DATE") || !strcmp(tok, "MMDDYY") || !strcmp(tok, "DDMMYY") ||
      !strcmp(tok, "YYMMDD") || !strcmp(tok, "JULIAN") || !strcmp(tok, "MONYY") ||
      !strcmp(tok, "WEEKDATE") || !strcmp(tok, "WORDDATE"))
    return DATE_DAILY;
  if (!strcmp(tok, "DATETIME")) return DATE_SAS_DT;
  if (!strcmp(tok, "TIME") || !strcmp(tok, "HHMM") || !strcmp(tok, "MMSS"))
    return DATE_TIME_SECS;
  return DATE_NONE;
}

// Howard Hinnant's civil-from-days: days since 1970-01-01 → (y, m, d). Exact for
// the full proleptic Gregorian range; no libc/timezone dependency.
static void civil_from_days(long z, int *yy, int *mm, int *dd) {
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = (unsigned)(z - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long y = (long)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp < 10 ? mp + 3 : mp - 9;
  *yy = (int)(y + (m <= 2));
  *mm = (int)m;
  *dd = (int)d;
}

// Howard Hinnant's days-from-civil: (y, m, d) → days since 1970-01-01.
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// A Stata period value → the first day of the period, as "yyyy-mm-dd".
static void sb_period_value(strbuf *sb, int kind, double value) {
  int64_t v = (int64_t)floor(value);
  int64_t year;
  unsigned month = 1, day = 1;
  int64_t extra_days = 0;
  switch (kind) {
    case DATE_WEEK: {  // 52 weeks a year; week k (0-based) starts on Jan 1 + 7k
      year = 1960 + floor_div(v, 52);
      extra_days = 7 * (v - 52 * (year - 1960));
      break;
    }
    case DATE_MONTH:
      year = 1960 + floor_div(v, 12);
      month = (unsigned)(v - 12 * (year - 1960)) + 1;
      break;
    case DATE_QUARTER:
      year = 1960 + floor_div(v, 4);
      month = 3 * (unsigned)(v - 4 * (year - 1960)) + 1;
      break;
    case DATE_HALF:
      year = 1960 + floor_div(v, 2);
      month = 6 * (unsigned)(v - 2 * (year - 1960)) + 1;
      break;
    default:  // DATE_YEAR
      year = v;
  }
  int y, m, d;
  civil_from_days((long)(days_from_civil(year, month, day) + extra_days), &y, &m, &d);
  char tmp[24];
  snprintf(tmp, sizeof tmp, "\"%04d-%02d-%02d\"", y, m, d);
  sb_puts(sb, tmp);
}

// Emit a numeric date/datetime/time value as a quoted ISO string per its kind.
// All arithmetic in int64_t — SPSS second-counts (~1.4e10 for modern dates) and
// Stata/SAS post-2038 second/ms counts overflow wasm32's 32-bit long. Uses
// floored division so pre-epoch (negative offset) values decode correctly.
static void sb_date_value(strbuf *sb, int kind, double value) {
  char tmp[48];
  if (kind >= DATE_WEEK) {
    sb_period_value(sb, kind, value);
    return;
  }
  if (kind == DATE_TIME_SECS) {
    // A duration since midnight, not a calendar instant. May be negative or
    // exceed 24h (SAS/SPSS times are durations); DuckDB types the in-range
    // common case as TIME, out-of-range columns fall back to VARCHAR.
    int64_t secs = (int64_t)value;  // truncate sub-second
    const char *sign = secs < 0 ? "-" : "";
    if (secs < 0) secs = -secs;
    snprintf(tmp, sizeof tmp, "\"%s%02d:%02d:%02d\"", sign, (int)(secs / 3600),
             (int)((secs / 60) % 60), (int)(secs % 60));
    sb_puts(sb, tmp);
    return;
  }
  int64_t days;
  int64_t tod = 0;      // seconds within the day (datetime kinds only)
  int64_t epoch_days;   // family epoch → 1970-01-01, in days
  int is_dt = 0;
  switch (kind) {
    case DATE_DATETIME:  // Stata %tc: ms since 1960
    case DATE_SAS_DT:    // SAS DATETIME: s since 1960
    case DATE_SPSS_DT: { // SPSS DATETIME: s since 1582
      int64_t secs = kind == DATE_DATETIME ? (int64_t)(value / 1000.0)  // ms → s
                                           : (int64_t)value;
      days = secs / 86400;
      tod = secs - days * 86400;
      if (tod < 0) {  // floor toward negative infinity
        tod += 86400;
        days -= 1;
      }
      epoch_days = kind == DATE_SPSS_DT ? SPSS_EPOCH_DAYS : STATA_EPOCH_DAYS;
      is_dt = 1;
      break;
    }
    case DATE_SPSS_DATE:  // SPSS day formats: s since 1582, midnight-aligned
      days = (int64_t)(value / 86400.0);
      epoch_days = SPSS_EPOCH_DAYS;
      break;
    default:  // DATE_DAILY — Stata %td / SAS day formats: days since 1960
      days = (int64_t)value;
      epoch_days = STATA_EPOCH_DAYS;
  }
  int y, m, d;
  civil_from_days((long)(days - epoch_days), &y, &m, &d);
  if (is_dt) {
    snprintf(tmp, sizeof tmp, "\"%04d-%02d-%02d %02d:%02d:%02d\"", y, m, d,
             (int)(tod / 3600), (int)((tod % 3600) / 60), (int)(tod % 60));
  } else {
    snprintf(tmp, sizeof tmp, "\"%04d-%02d-%02d\"", y, m, d);
  }
  sb_puts(sb, tmp);
}

static int meta_handler(readstat_metadata_t *md, void *ctx) {
  (void)ctx;
  g.row_count = readstat_get_row_count(md);
  int vc = readstat_get_var_count(md);
  g.var_count = vc;
  // a NULL calloc previously left g.names NULL, so the next var_handler's
  // `g.names[index] = …` was a NULL-deref. Flag OOM and abort the parse instead.
  int n = vc > 0 ? vc : 1;
  g.names = calloc(n, sizeof(char *));
  g.datekind = calloc(n, sizeof(int));  // DATE_NONE (0) for every column by default
  g.formats = calloc(n, sizeof(char *));
  g.labels = calloc(n, sizeof(char *));
  if (!g.names || !g.datekind || !g.formats || !g.labels) {
    g.var_count = 0;
    g.oom = 1;
    return READSTAT_HANDLER_ABORT;
  }
  return READSTAT_HANDLER_OK;
}

static int var_handler(int index, readstat_variable_t *var, const char *labels, void *ctx) {
  (void)labels;
  (void)ctx;
  const char *name = readstat_variable_get_name(var);
  if (index >= 0 && index < g.var_count) {
    // check strdup — on OOM, abort rather than storing NULL silently.
    char *dup = strdup(name ? name : "");
    if (!dup) {
      g.oom = 1;
      return READSTAT_HANDLER_ABORT;
    }
    g.names[index] = dup;
    // Remember which columns carry a recognized date/time display format so
    // value_handler can decode their numeric offset to an ISO string. The
    // classifier is per format family: 0=dta 1=sav 2=por 3=sas7bdat 4=xport.
    const char *vfmt = readstat_variable_get_format(var);
    const char *vlabel = readstat_variable_get_label(var);
    g.formats[index] = strdup(vfmt ? vfmt : "");
    g.labels[index] = strdup(vlabel ? vlabel : "");
    if (!g.formats[index] || !g.labels[index]) {
      g.oom = 1;
      return READSTAT_HANDLER_ABORT;
    }
    if (g.fmt == 0) g.datekind[index] = stata_date_kind(vfmt);
    else if (g.fmt == 1 || g.fmt == 2) g.datekind[index] = spss_date_kind(vfmt);
    else if (g.fmt == 3 || g.fmt == 4) g.datekind[index] = sas_date_kind(vfmt);
  }
  return READSTAT_HANDLER_OK;
}

static void flush_row(void) {
  if (g.started_row) {
    sb_puts(&g.out, "}\n");
    g.started_row = 0;
  }
}

static int value_handler(int obs_index, readstat_variable_t *var, readstat_value_t value,
                         void *ctx) {
  (void)ctx;
  if (obs_index != g.cur_obs) {
    flush_row();
    g.cur_obs = obs_index;
  }
  if (!g.started_row) {
    sb_putc(&g.out, '{');
    g.started_row = 1;
  } else {
    sb_putc(&g.out, ',');
  }
  int vindex = readstat_variable_get_index(var);
  const char *name =
      (vindex >= 0 && vindex < g.var_count && g.names[vindex]) ? g.names[vindex]
                                                               : readstat_variable_get_name(var);
  sb_json_str(&g.out, name ? name : "");
  sb_putc(&g.out, ':');

  if (readstat_value_is_missing(value, var)) {
    sb_puts(&g.out, "null");
    return READSTAT_HANDLER_OK;
  }
  // Date/time column: decode the numeric offset to an ISO string. Falls
  // through to normal emission for a non-numeric/NaN value.
  int dk = (vindex >= 0 && vindex < g.var_count) ? g.datekind[vindex] : DATE_NONE;
  if (dk != DATE_NONE) {
    int isnum = 1;
    double num;
    switch (readstat_value_type(value)) {
      case READSTAT_TYPE_INT8: num = readstat_int8_value(value); break;
      case READSTAT_TYPE_INT16: num = readstat_int16_value(value); break;
      case READSTAT_TYPE_INT32: num = readstat_int32_value(value); break;
      case READSTAT_TYPE_FLOAT: num = readstat_float_value(value); break;
      case READSTAT_TYPE_DOUBLE: num = readstat_double_value(value); break;
      default:
        isnum = 0;
        num = 0;
    }
    if (isnum && !isnan(num) && !isinf(num)) {
      sb_date_value(&g.out, dk, num);
      return READSTAT_HANDLER_OK;
    }
  }
  char tmp[64];
  switch (readstat_value_type(value)) {
    case READSTAT_TYPE_STRING:
    case READSTAT_TYPE_STRING_REF:
      sb_json_str(&g.out, readstat_string_value(value));
      break;
    case READSTAT_TYPE_INT8:
      snprintf(tmp, sizeof tmp, "%d", readstat_int8_value(value));
      sb_puts(&g.out, tmp);
      break;
    case READSTAT_TYPE_INT16:
      snprintf(tmp, sizeof tmp, "%d", readstat_int16_value(value));
      sb_puts(&g.out, tmp);
      break;
    case READSTAT_TYPE_INT32:
      snprintf(tmp, sizeof tmp, "%d", readstat_int32_value(value));
      sb_puts(&g.out, tmp);
      break;
    case READSTAT_TYPE_FLOAT: {
      float f = readstat_float_value(value);
      if (isnan(f) || isinf(f)) {
        sb_puts(&g.out, "null");
      } else {
        // %.7g = float32's ~7 significant decimal digits, so 3.58f prints
        // as "3.58" not "3.57999992" (float representation artifact).
        snprintf(tmp, sizeof tmp, "%.7g", (double)f);
        sb_puts(&g.out, tmp);
      }
      break;
    }
    case READSTAT_TYPE_DOUBLE: {
      double d = readstat_double_value(value);
      if (isnan(d) || isinf(d)) {
        sb_puts(&g.out, "null");
      } else {
        snprintf(tmp, sizeof tmp, "%.17g", d);
        sb_puts(&g.out, tmp);
      }
      break;
    }
    default:
      sb_puts(&g.out, "null");
  }
  return READSTAT_HANDLER_OK;
}

// L25: release the wasm-resident output of the previous parse (NDJSON + column
// list + name table). Exported so the JS side can free the last file's buffer
// after copying it out — otherwise it stays resident for the whole session.
void rs_free(void) {
  if (g.out.buf) free(g.out.buf);
  if (g.cols.buf) free(g.cols.buf);
  if (g.names) {
    for (int i = 0; i < g.var_count; i++) free(g.names[i]);
    free(g.names);
  }
  if (g.formats) {
    for (int i = 0; i < g.var_count; i++) free(g.formats[i]);
    free(g.formats);
  }
  if (g.labels) {
    for (int i = 0; i < g.var_count; i++) free(g.labels[i]);
    free(g.labels);
  }
  if (g.meta.buf) free(g.meta.buf);
  if (g.datekind) free(g.datekind);
  memset(&g, 0, sizeof g);
  g.cur_obs = -1;
}

// format: 0=dta 1=sav 2=por 3=sas7bdat 4=xport. Returns 0 on success, -1 on a
// file-write failure, -2 on an out-of-memory while building output, else the
// readstat_error_t code.
int rs_read(const uint8_t *data, int len, int format) {
  rs_free();  // free any prior run
  g.fmt = format;  // selects the date-format classifier (rs_free memset cleared it)

  FILE *f = fopen("/rs_input", "wb");
  if (!f) return -1;
  fwrite(data, 1, len, f);
  fclose(f);

  readstat_parser_t *parser = readstat_parser_init();
  readstat_set_metadata_handler(parser, meta_handler);
  readstat_set_variable_handler(parser, var_handler);
  readstat_set_value_handler(parser, value_handler);

  readstat_error_t err;
  switch (format) {
    case 0: err = readstat_parse_dta(parser, "/rs_input", NULL); break;
    case 1: err = readstat_parse_sav(parser, "/rs_input", NULL); break;
    case 2: err = readstat_parse_por(parser, "/rs_input", NULL); break;
    case 3: err = readstat_parse_sas7bdat(parser, "/rs_input", NULL); break;
    case 4: err = readstat_parse_xport(parser, "/rs_input", NULL); break;
    default: err = READSTAT_ERROR_PARSE;
  }
  flush_row();
  readstat_parser_free(parser);
  remove("/rs_input");

  sb_putc(&g.cols, '[');
  for (int i = 0; i < g.var_count; i++) {
    if (i) sb_putc(&g.cols, ',');
    sb_json_str(&g.cols, g.names[i] ? g.names[i] : "");
  }
  sb_putc(&g.cols, ']');

  sb_putc(&g.meta, '[');
  for (int i = 0; i < g.var_count; i++) {
    if (i) sb_putc(&g.meta, ',');
    sb_puts(&g.meta, "{\"format\":");
    sb_json_str(&g.meta, g.formats && g.formats[i] ? g.formats[i] : "");
    sb_puts(&g.meta, ",\"label\":");
    sb_json_str(&g.meta, g.labels && g.labels[i] ? g.labels[i] : "");
    sb_putc(&g.meta, '}');
  }
  sb_putc(&g.meta, ']');

  // an allocation failed while building the name table or the output
  // buffers — report it distinctly instead of returning a truncated success.
  if (g.oom || g.out.oom || g.cols.oom || g.meta.oom) return -2;

  return err == READSTAT_OK ? 0 : (int)err;
}

const char *rs_ndjson(void) { return g.out.buf ? g.out.buf : ""; }
int rs_ndjson_len(void) { return (int)g.out.len; }
const char *rs_columns(void) { return g.cols.buf ? g.cols.buf : "[]"; }
const char *rs_meta(void) { return g.meta.buf ? g.meta.buf : "[]"; }
int rs_rowcount(void) { return g.row_count; }
int rs_varcount(void) { return g.var_count; }
const char *rs_errmsg(int code) { return readstat_error_message((readstat_error_t)code); }
