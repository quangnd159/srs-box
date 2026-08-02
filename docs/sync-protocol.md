# Sync protocol

File transfer over the same `@`-prefixed CDC link as `@shot`/`@tap` (see
`tools/devctl.py` and `firmware/components/devctl/`). Two clients speak it:
`./dev` (pyserial) and the web app (WebSerial). The firmware is the single
implementation both talk to.

## Device file layout

LittleFS is mounted at `/data` (partition `storage`, 11MB).

| Path | Direction | Notes |
|---|---|---|
| `/data/decks/<slug>.srs` | host → device | one deck per file; replaced wholesale on sync |
| `/data/fonts/font_cjk_<size>.bin` | host → device | LVGL binary fonts (sizes 16, 20, 28, 48), subset over the **union** of all installed decks' glyphs; replaced together on any deck sync |
| `/data/revlog.bin` | device → host | global append-only review log; **never written by sync** |
| `/data/lastknowntime.bin` | device only | no-RTC time fallback |

Card ids embed the deck slug (see `docs/deck-format.md`), so one global
revlog serves every deck; replaying it into a deck's session simply skips
entries whose card id is not in that deck.

If `/data/decks/` is empty the firmware falls back to the deck embedded in
the app image, and to the compiled-in fonts. Fonts on flash win over
compiled-in fonts when present.

## Commands

Line-based, LF-terminated, ASCII. Replies are `@ok ...` / `@err ...` as
today. Paths are **relative to `/data`**, must match
`[A-Za-z0-9._/-]+`, must not contain `..`, and only `decks/`, `fonts/`,
and `revlog.bin` are addressable.

Both clients send `@time <unix_epoch_seconds>` automatically before the
first command of any of the above (web app: right after WebSerial connects;
`./dev`/`devctl.py`: once at the start of each `push`/`pull`/`ls`/`rm`/
`sync`/`reboot` invocation), so every connection re-anchors the device's
clock without the user having to remember `./dev synctime`. That auto-sync
is best-effort and never blocks the requested operation on failure.

```
@fput <path> <nbytes> <crc32>     push a file
@fget <path>                      pull a file
@fls                              list files
@fdel <path>                      delete (decks/ and fonts/ only)
@reboot                           apply pushed content by restarting
```

### `@fput`

1. Host sends `@fput decks/hsk1.srs 45120 9a3e11c2`. `crc32` is IEEE
   CRC-32 (zlib), lowercase hex, over the file bytes.
2. Device validates path, size (max 4MB), and free space, then replies
   `@ok send`. On any problem: `@err <reason>` and nothing further.
3. Host streams exactly `nbytes` of raw data. The device buffers the whole
   payload in PSRAM and touches flash only after the stream ends. This is
   load-bearing: writing LittleFS mid-stream stalls the USB drain during
   block erases, and a NAK lasting more than a few seconds makes macOS drop
   the in-flight tail silently. If no bytes arrive for 10 s the device
   replies `@err timeout after <got>/<want> bytes`.
4. On the final byte the device verifies the CRC, creates parent
   directories, renames the staging file onto the target (atomic in
   LittleFS), and replies `@ok fput <path> <nbytes>`. CRC mismatch:
   `@err crc mismatch` and the target is left untouched.

Pushed content is picked up at boot, not hot-reloaded: after pushing
decks and fonts the host sends `@reboot`.

### `@fget`

Device replies `@fget <nbytes> <crc32>\n` followed by exactly `nbytes`
of raw data, with ESP_LOG muted for the duration exactly as `@shot`
does. Missing file: `@err not found`.

This is how the host pulls `revlog.bin` for Anki export. v1 never
truncates the log after export; truncation gets its own command later
once export round-trips are proven.

### `@fls`

Single-line reply: `@ok fls <path>=<size> <path>=<size> ...` covering
`decks/`, `fonts/`, and `revlog.bin`. Reply buffer is 512 bytes; if the
listing would overflow, it is truncated at a whole entry and ends with
`...`.

### `@reboot`

Replies `@ok rebooting`, waits ~200 ms for the reply to flush, then
`esp_restart()`.

## Host-side commands

```
./dev push <local-file> <device-path>    @fput
./dev pull <device-path> [local-file]    @fget
./dev ls                                 @fls
./dev sync <deck.srs>...                 push decks + rebuilt fonts, then @reboot
./dev reboot                             @reboot
```

## Deck metadata additions

`deckc.py` (and the web app's TS port, which must produce byte-identical
output for identical input) writes these keys in the `META` section:

```
name=<display name>        shown on the deck-picker screen
slug=<stable slug>
cards=<count>
lang=<bcp47-ish tag>       "zh" for Chinese, "fr" for French, ...
```

`lang` gates language-specific rendering: pinyin tone-colouring of the
reading line applies only when `lang=zh`. For other languages the
reading (e.g. French IPA) renders in a single neutral colour.

Non-Chinese decks compile from a 3-column TSV: `front <TAB> reading
<TAB> back` (reading may be empty). The 5-column hskhsk format remains
supported for Chinese, and its numeric-pinyin column is what the syllable
separator in the reading field is derived from (see docs/deck-format.md).
