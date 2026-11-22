# Battery stats in the firmware: design (approved; built as patch 0025)

Goal: measured battery numbers on the Xteink X4 Pro, X4 Classic and X3, written in the
Android companion's export format `orbit-battery-stats/1`
(BookOrbit-Android `docs/STATS-DESIGN.md`, "Export JSON"). The two can then be compared side by
side. Constraint: memory must not grow much. Approved with the recommended tiers; built as
`overlay/patches/v2.0/0025-battery-stats.patch`. Where the build differs from this design,
§9 "As built" wins.

Code references are to upstream CrossInk-Bookorbit `v2.0.0+bookorbit.1-rc2` (`src/`, `lib/`) and
its FreeInk SDK at `aad0749` (`freeink-sdk/libs/...`), unless a line says otherwise. Hooks
with the same names exist in `v1.6.1+bookorbit.8`.

## 1. What the firmware already has (findings)

### Battery reading
| Reader | Chip | Backend | % resolution | Raw mV |
|---|---|---|---|---|
| X3 | ESP32-C3, no PSRAM | BQ27220 fuel gauge, 650 mAh (`BoardConfig.h:1096`) | integer % (`BatteryMonitor.cpp:501`) | yes, gauge `Voltage()` (`BatteryMonitor.cpp:524-526`) |
| X4 Pro | ESP32-S3, 8 MB PSRAM | CW2017 fuel gauge (`BoardConfig.h:1978`), no design capacity | integer % (reg 0x04; the fraction reg 0x05 is unused, `BatteryMonitor.cpp:242`) | yes, 14-bit VCELL, ~0.31 mV/LSB (`BatteryMonitor.cpp:516-522`) |
| X4 Classic | ESP32-S3, 8 MB PSRAM | CW2017 (`BoardConfig.h:2082`) | integer % | yes |
| X4 (original; same `default` binary as the X3) | ESP32-C3 | ADC on GPIO0 with a 2:1 divider (`HalGPIO.h:19`, `BoardConfig.h:1031-1045`) | **10 % steps**, on purpose (`BatteryMonitor.h:94-99`) | yes, `analogReadMilliVolts` x 2 (`BatteryMonitor.cpp:888-940`) |

- **API:** `BatteryMonitor::readStatus()` returns percent, mV, charging and external power, each with
  a `Known` flag (`BatteryMonitor.h:21-62`). `readMillivolts()` is at `BatteryMonitor.h:65`.
- **How often:** only when something asks. `HalPowerManager::getBatteryPercentage()` caches for
  `BATTERY_POLL_MS` = 30 s on 2.x (`HalPowerManager.h:55`, `.cpp:309-338`; 1.5 s on 1.6.1). The
  ADC path is smoothed 9:1 (`.cpp:332-337`). On USB power the main loop polls once a minute
  (`main.cpp:2488-2500`). Nothing samples while the device sleeps.
- **Charging:**
  - X3: inferred from the sign of the gauge current (`HalGPIO.cpp:408-415`);
  - S3 readers: the charge-STAT line, polled at most every 500 ms (`HalGPIO.cpp:424-447`);
  - all: plug changes reach the loop through `gpio.wasUsbStateChanged()` (`main.cpp:2484`).
- **Prior art:** `src/util/BatteryDiagnosticLog.{h,cpp}` appends a CSV row (soc, mV, charging,
  device, wake route) on every wake (`main.cpp:1834`) and sleep (`main.cpp:1352`). It is compiled
  out of every shipping env (`CROSSINK_BATTERY_DIAG_LOG`, `HalPowerManager.h:18-23`). Its rows are
  ~150 B, CSV, with no counters. This design reuses its two call sites and its rule that a failed
  write is never fatal (`BatteryDiagnosticLog.cpp:152-227`).

### Time
- **X3:** DS3231 RTC (`BoardConfig.h:1098`). **X4 Pro / X4 Classic:** BM8563 RTC, which is
  PCF8563-compatible (`BoardConfig.h:1984`, `:2088`; `FREEINK_CAP_RTC` at `BoardConfig.h:363-367`).
  At boot the RTC is read into the system clock (`main.cpp:1733-1747`, `trustedtime::adopt`).
  **Wall-clock time is known with no Wi-Fi**, once the RTC has been set by NTP or Clock Sync.
- **Original X4 (no RTC):** `WallClock` (`lib/WallClock/WallClock.h:5-80`) keeps time and
  checkpoints to `/.crosspoint/wallclock.bin` at each sleep (`main.cpp:1532`). When the clock is
  lost, a new power "era" starts, and the error is corrected after the next NTP sync
  (`correctionForEvent`). `BookOrbitStatsQueue::captureNow()` is the existing helper that returns
  epoch seconds plus an "approximate" flag (`lib/BookOrbitSync/BookOrbitStatsQueue.h:76-82`).
  With `keepClockInSleep` off, sleep releases the power latch and the C3 board turns off
  (`HalPowerManager.cpp:186-194`). The original X4 therefore cannot measure how long it slept
  until a later sync corrects that era.
- `trustedtime::trustedNow()` returns 0 when the time is not trustworthy (`TrustedTime.h:47-51`).

### Hooks
| Event | Where |
|---|---|
| Wake / boot (after the SD mounts) | `main.cpp:1834`, beside `BatteryDiagnosticLog::record(Wake)`. Reset reason and wake cause are at `main.cpp:1615-1616`. A sleep-sync boot is `sleepSyncBoot` (`main.cpp:1679`). |
| Sleep (last SD write) | `powerDownForDeepSleep()`, `main.cpp:1352`. Both ordinary sleep and sleep after the sleep sync pass through it (`main.cpp:1488`, `:1492-1540`). |
| Software restart into a network boot | `restartWithSilentToken()`, `main.cpp:417` |
| Page turns | EPUB `EpubReaderActivity::pageTurn` (`EpubReaderActivity.cpp:6649`); TXT/MD `TxtReaderActivity::requestPageTurn` (`TxtReaderActivity.cpp:326`); XTC/XTCH `XtcReaderActivity.cpp:519/529` |
| Full vs fast refresh | `HalDisplay::displayBuffer`, `refreshDisplay` and `displayBufferAsync` take a `RefreshMode` of FULL, HALF or FAST (`lib/hal/HalDisplay.h:22-26`, `.cpp:121`, `:145`, `:172`). `displayGrayBuffer` is at `.cpp:240`. |
| Content type | `ReaderActivity::onEnter` (`ReaderActivity.cpp:181`) dispatches on extension: XTC/XTCH (`:21`), TXT/MD (`:23-26`), otherwise EPUB. There is no PDF or CBZ reader. |
| Wi-Fi on/off | about 25 scattered `WiFi.mode(WIFI_STA/OFF)` sites (`SavedWifiConnect.cpp:132/166`, `WifiSelectionActivity.cpp`, …). One `WiFi.onEvent` handler for STA_START/STA_STOP covers them all. |
| Sync | Sleep sync: `BookOrbitSyncActivity::runSleepSync` (`BookOrbitSyncActivity.cpp:328`). Manual sync: `performSync` (`:417`). Sync While Reading: overlay patch `v2.0/0022` (`BookOrbitBackgroundSync.cpp`). |
| Frontlight (X4 Pro only; the X4 Classic has none, `BoardConfig.h:2074`) | `HalFrontlight::setOn` and `setBrightness`; `isOn()` and `brightness()` (0-100) (`lib/hal/HalFrontlight.h:25-32`) |
| Plug / unplug while awake | `gpio.wasUsbStateChanged()`, `main.cpp:2484` |

### SD card
- Mounted at boot by `Storage.begin()` (`main.cpp:1803`). Unmounted at sleep by `Storage.shutdown()`
  (`main.cpp:1356`); on the X3 the SD rail is cut (`main.cpp:1350`).
- Small-file writers already run at every sleep: `APP_STATE.saveToFile()`, `WallClock::checkpoint()`
  and the library scan token. One more 16-byte append adds almost nothing to card wear.
- Fixed-size binary append with a header and a cap: `BookOrbitStatsQueue::appendBatch` (16-byte
  records, `BookOrbitStatsQueue.h:21-54`). Append and close with sync on the sleep path:
  `BatteryDiagnosticLog.cpp:159-226`.

### Persistent state
- `RTC_NOINIT_ATTR` with a magic number and checksum is the house pattern for state carried across
  restarts and deep sleep (`lib/hal/BatteryEstimateRetention.h:5-37`; `main.cpp:365-376`).
- NVS (`Preferences`) is used by `TrustedTime`.
- On an S3, RTC memory survives deep sleep. On a C3 whose latch was released, it does not (the
  board turns off). It never survives a power loss. **Each transition therefore writes its row
  to SD; RTC memory only carries in-flight counters across software restarts.**

## 2. What is recorded

**One append-only file, `/.crossink/battery.bin`.** It has an 8-byte header (`"OBS1"`, version,
board id) followed by 16-byte records. One record is written per transition and never on a timer.

| Bytes | Field |
|---|---|
| 4 | `t`: UTC epoch seconds, from `BookOrbitStatsQueue::captureNow()` |
| 1 | `kind`: `wake`, `wake_sync` (sleep-sync boot), `wake_net` (network boot), `cold_boot`, `sleep`, `restart`, `level` (1 % step), `wifi_on`, `wifi_off`, `charge_on`, `charge_off`, `book_open`, `book_close`, `light` |
| 1 | `era`: WallClock era, truncated |
| 1 | `flags`: clock approximate, charging, USB, Wi-Fi on, light on, 2 bits of content (none/text/xtc) |
| 1 | `pct`: battery % |
| 2 | `mv`: battery mV, 0 = unknown |
| 1 | `light_level`: 0-100 (X4 Pro) |
| 1 | `full`: full and half refreshes since the last record (saturating) |
| 2 | `pages`: page turns since the last record |
| 2 | `partial`: fast and grayscale refreshes since the last record |

- **In-RAM counters** (pages, full, partial, the last pct, the Wi-Fi state, and two pending
  Wi-Fi events) live in one 32-byte `RTC_NOINIT` struct with a checksum. That is RTC memory, not
  DRAM. They are written into every record and then zeroed. After a cold boot the checksum fails,
  so the counts start at zero and a `cold_boot` record is written.
- **`level` records:** when the cached percent from `getBatteryPercentage()` changes. That
  function only runs when the UI already asks for it (status bar, home), so **no new wakeups and
  no new polling**. They give the reading segments their inner points, as the companion's
  `ACTION_BATTERY_CHANGED` rows do.
- **Wi-Fi:** the event handler runs in the Arduino event task. It only stores the timestamp and
  state in the pending slots, and the main loop appends the record (one line in `loop()`). It
  never writes to the SD card from the event task.
- **Charging while asleep** cannot be seen. A sleep-to-wake interval whose percent rose, or which
  ended on USB, is classed as charging and kept out of the standby rates.
- **Size:** about 100 records a day x 16 B = 1.6 KB a day, about 0.6 MB a year. The file rolls
  to `battery.1.bin` at 4 MB (about 7 years). Nothing is ever deleted.

## 3. Export (`orbit-battery-stats/1`)

- **Trigger:** a menu item (Settings > System > Battery stats > Export) writes
  `/battery_stats.json`. On the S3 readers it is also written when USB Drive mode starts
  (`UsbDriveActivity.cpp:44`), so the file is current when the card shows up on a computer. It
  is not written at sync: the X3 sync is tight on heap and the sleep sync is time-critical. The
  file can also be fetched through the existing web file manager.
- **Computation:** one streaming pass over `battery.bin`, using the companion's exact
  definitions:
  - segments: maximal runs with the same state, Wi-Fi, content and light;
  - a pooled least-squares slope with an intercept per segment, and its standard error;
  - the companion's thresholds (1 h, > 2 %, 100 pages, 24 h for the time-left estimate).

  Each category keeps O(1) sums (n, Σt, Σy, Σtt, Σty folded per segment): at most 16
  categories x ~40 B. `daily` rows are written as each local day closes, so nothing is held.
  JSON is printed with `snprintf` straight into the file, with no ArduinoJson document.
- **States:** `standby` = asleep, not charging. `reading` = awake with a book open, not charging.
  `charging` = on USB with the percent rising. Awake without a book (Home, menus) counts in
  `daily.screen_on_min` only, as on Android, where it is screen-on time outside KOReader.
- **Records left out:**
  - records with the clock-approximate flag whose era was never corrected (the original X4 only);
  - X4 (ADC) values come from `mv` through an interpolated curve, not the 10 % steps.
- **Fields:**
  - `source`: `{ "app": "crossink-firmware", "version": AppVersion::version() }`
  - `device`: `{ "manufacturer": "Xteink", "model": "X4 Pro" | "X4 Classic" | "X3" | "X4", "battery_source": "fuel_gauge" | "voltage_estimate", "capacity_mah": 650 | null, "capacity_source": "board_profile" | null }`
    - `android` is left out.
    - `battery_source` is `fuel_gauge` on the X3, X4 Pro and X4 Classic, and `voltage_estimate`
      on the original X4.
  - `rates`, `per_100` and `daily` as in /1. `wifi_mode` is `null`. `light` is `"on"`/`"off"`
    on the X4 Pro and `null` elsewhere.

### Schema notes for the Android side (it is freezing /1)
Additive fields are safe after the freeze ("unknown fields are ignored"). Value vocabularies and
meanings are not. **Urgent (needed before the freeze):**
1. **Unknown enum values must be shown verbatim, never rejected.** The firmware then emits
   `content: "xtc"` for pre-rendered XTC books instead of mislabelling them `text` or `comic`.
   This applies to `content`, `state`, `unit` and `light`.
2. **`light` vocabulary:** `"on"` / `"off"` / null. /1 shows only `null`.
3. **`per_100.unit` vocabulary:** `"pages"` and `"full_refreshes"`. /1 shows only `pages`,
   although the design defines "per 100 full refreshes".
4. **`generated` and `period` must allow null** when the device has no trusted date (an unset RTC).
5. **Meanings:** for non-Android sources, `standby` means "device idle/asleep" and
   `screen_on_min` means "awake minutes".

**Additive (any time):** `device.battery_source`, `source.app = "crossink-firmware"`, and an
optional `wifi_radio` ("on"/"off") on `rates`. The companion computes standby by radio on/off,
but its export row cannot carry it.

## 4. On-device UI options

- **None (export only).** Read the JSON on the phone.
- **Minimal page:** four lines with the companion's "—" rule:
  - time left, standby %/h, reading %/h, and % per 100 pages;
  - the age of the data;
  - an Export button.

  It reuses the export's pass (it computes the same numbers and writes none of them), so the
  page itself only draws. English strings only; the other 28 languages fall back to English
  (`ENGLISH_FALLBACK_OFFSET`, `scripts/gen_i18n.py:40`).

## 5. Cost

**Method:** these are not compiled. They are estimated from linked sizes in the rc2 `.map` files
(`c3work/rc2port/crossink/.pio/build/{default,x4-pro}/firmware.map`, `.text` + `.rodata` +
`.data` of each object):

| Comparable module | Linked size on default | Linked size on x4-pro |
|---|---|---|
| `WallClock` | 2.2 KB | 2.0 KB |
| `BookOrbitStatsQueue` | 3.9 KB | 3.6 KB |
| `StatsBackup` | 4.9 KB | 4.2 KB |
| `StatsUploadPayload` | 2.4 KB | 2.0 KB |
| `BackupStatsActivity` | 1.3 KB | 1.4 KB |
| `ClockSyncActivity` | 2.2 KB | 2.0 KB |
| `BookStatsView` (full chart page; upper bound) | 9.9 KB | 10.6 KB |

Treat the estimates below as ±50 %.

| Tier | Flash | Static RAM | Heap / stack |
|---|---|---|---|
| 1 Record (log, counters, hooks) | ~2.5 KB | 32 B RTC_NOINIT, ~16 B DRAM | ~150 B of stack per append, none kept |
| 2 + Export (menu item, JSON) | +~5 KB | 0 | ~1.2 KB, only while exporting |
| 3 + Stats page | +~4 KB (incl. ~0.5 KB of strings) | 0 | page object ~200 B while open |

Headroom is from the 11-21 `features/*-final`/`-fw` builds:

| Reader (env) | App flash free now | Tier 3 (~11.5 KB) | Tier 2 (~7.5 KB) | Static RAM now |
|---|---|---|---|---|
| X4 Pro (`x4-pro`) | 430 KB (93.4 %) | 2.7 % of free | 1.7 % | 137 KB |
| X4 Classic (`x4-classic`) | 580 KB (91.2 %) | 2.0 % | 1.3 % | 134 KB |
| X3 + X4 (`default`) | 92 KB (98.6 %) | 12.5 % | 8.1 % | 64 KB |

**Recommendation:**
- **X4 Pro and X4 Classic: tier 3.**
- **`default` (X3 and the original X4): tier 2.** Record, plus export from the menu: no page and
  no USB-drive trigger.
- Tier 1 is the fallback if a later 2.x cut needs the 5 KB back. Recording alone is invisible to
  the user, so tier 0 (off) is the better alternative to tier 1.
- The original X4 gets the feature for free, with `battery_source: voltage_estimate`. Its
  standby rates appear only after syncs correct the era.

## 6. Switch-table entry (`tailnet/tailnet_build.py`)

```python
    # Patch 0025: battery stats. 0 off, 1 record, 2 + export (menu), 3 + stats page and USB-drive export.
    "CROSSINK_TAILNET_BATTERY_STATS":     (2,      3,      3),
```

- Added to `V2_ONLY_SWITCHES`.
- The code is `#if CROSSINK_TAILNET_BATTERY_STATS >= n`. At 0 every hook is an empty inline
  (the `BatteryDiagnosticLog.h:60-62` pattern), so it compiles out completely.
- Patch: `overlay/patches/v2.0/0025-battery-stats.patch`, adding `src/util/BatteryStats.{h,cpp}`
  and one-line hooks at the sites in §1.

## 7. Decisions I made (two-way doors)

- **2.x only** (patch series v2.0). 1.6.1 is the stable line and gets no new features. The hooks
  are the same, so a backport is a copy.
- **Binary 16-byte records, not CSV.** A row is about a tenth of the size, and it follows the
  `BookOrbitStatsQueue` precedent. The log is internal; its version is in the header.
- **No rollup store.** The whole log is read at export, which takes well under a second at a
  year's size. This saves a second file and its code.
- **No periodic wakeups.** Level points come only from polls the UI already makes, and sleep
  intervals give two points each.
- **No user setting.** The switch is the control, and a setting costs strings and NVS on the X3.
- **Hook through Wi-Fi events rather than ~25 call sites.** SD writes are deferred to the main loop.
- **Full = FULL + HALF refresh** (both flash). **Partial = FAST + grayscale**, matching the
  companion's flashing/non-flashing split.
- **Counting:** page turns count both directions, as the companion's `onPageUpdate` does.
  Footnote-preview paging counts too.
- **Content:** EPUB/TXT/MD → `text`; XTC/XTCH → `xtc`, which needs urgent item 1. If Android
  declines, they map to `comic`, since both are pre-rendered bitmaps.
- **Export triggers:** the menu, plus USB Drive on the S3 readers; not at sync (§3).
- **Export path:** `/battery_stats.json` at the card root, so it is visible over USB and in the
  web file manager. The log lives in the hidden `/.crossink/`.
- **The X3 gets tier 2, not tier 1.** Recording with no way to read it is cost without value. If
  space is ever needed, the switch drops it to 0.
- **The thresholds and the 7-day window are the companion's,** so the numbers compare like for like.

## 8. One-way doors for you

**1. Ask the Android team to put the urgent schema items (§3, 1-5) into /1 before it freezes.**
- **Options:**
  - (a) Send them now (recommended).
  - (b) Firmware maps everything onto today's /1 values: XTC → `comic`, no refresh-based
    `per_100`, a placeholder date when there is no clock.
  - (c) Wait and do it in `/2`.
- **Why it is hard to reverse:** once the companion and exported files exist, changing the
  vocabulary or meaning of an enum needs `/2`, and every reader must handle both versions.
  Today it costs one edit to a design doc.

The rest is reversible:
- the log format is internal and versioned;
- no data is ever deleted, because the log rolls and is never pruned;
- the export carries aggregates only, which the Android doc already settled (its door 1).

## 9. As built (patch 0025)

**Measured sizes:** local `pio run` of origin/main (`a01b358`) against origin/main plus
patch 0025, from the same CrossInk rc2 checkout:

| Env | Tier | App flash | Static RAM | App slot free after |
|---|---|---|---|---|
| `default` (X3 + X4) | 2 | +12,036 B | +72 B | 64,272 B (was 76,320) |
| `x4-pro` | 3 | +11,880 B | +72 B | 409,088 B |
| `x4-classic` | 3 | +11,740 B | +72 B | 561,792 B |

- On `default`, ~8.4 KB of the total is the analysis and JSON code (`BatteryStatsCore`). The C3
  has no FPU, so the double-precision sums are soft-float calls.
- The heap is used only while exporting. The JSON line buffer is 256 B on the stack, and the
  analyzer is about 0.7 KB on the stack, twice.
- The design's estimate was ~7.5 KB / ~11.5 KB with no static RAM. The `default` build costs
  more than estimated, and §5's "92 KB free" was the linker's figure; the image's real headroom
  was 76 KB.

**Differences from §2–§6:**
- **No `RTC_NOINIT` counters.**
  - Counts live in plain RAM and go into the next record.
  - A `restart` record is written before every software restart (`restartWithSilentToken`).
  - The `boot` record carries the reset class: continue, power-on or crash.
  - The export works out from the records alone whether a stretch was asleep, awake or unknown.
  - Counts in flight are lost only on a crash or power loss.
- **Polling in `loop()` instead of hooks:**
  - Book open/close, USB power and frontlight are checked once a second.
  - Battery % is checked once a minute through the power manager's 30-s cache.
  - None of this runs while the device sleeps, and none of it is a wakeup.
- **One page-turn hook:** `PluginReaderEvents::noteTurn`, which EPUB, TXT and XTC all call.
  EPUB footnote-preview paging is not counted.
- **Wi-Fi:** `WiFi.onEvent` (STA_START/STA_STOP) sets a flag, and `loop()` writes the record.
- **Refreshes:** counted in `HalDisplay` (`takeRefreshCounts`).
- **The schema follows the frozen `docs/STATS.md`** rather than §3's draft:
  - `source` is a string, plus `source_version`;
  - `battery_source` is at the top level;
  - rates carry `change_pct`;
  - the `radio` dimension and the `awake` state are used;
  - `segments` covers the last 90 days, and its times are null without an RTC.
- **The page:** opening Battery stats writes the export straight away, with no separate button.
  - Tier 3 shows time left, standby %/h, reading %/h and % per 100 pages, using the
    companion's thresholds.
  - Tier 2 shows only where the file was saved.
- **Log size:** the log rolls to `battery.1.bin` at 4 MB. A second roll (after about 14 years)
  replaces the old `battery.1.bin`.
- **Day attribution:** daily counts and drops land on the local day on which a stretch ends;
  minutes are split at midnight.
- **Tests:** host tests are in `test/battery_stats` (gtest, upstream harness). They cover:
  - record encoding;
  - ISO times;
  - the pooled rates;
  - stretches marked unknown;
  - the JSON;
  - an export with no clock.

  A sample export was checked against the rules in `docs/STATS.md`.
