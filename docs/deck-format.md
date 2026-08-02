# Deck format

## The one rule

**Content and scheduling state are separate files, and they are synced in opposite directions.**

Content flows host → device and is fully replaceable. Scheduling state and review history flow device → host and are never overwritten by a sync. They are joined by a stable card id.

This is the difference between a device you can iterate on and one you are afraid to touch. Fix a typo in a card, recompile the deck, push it, and your months of review history survive because the sync never writes to the state file. Anki draws the same line between notes, cards, and the revlog, and it is the reason Anki decks are safe to edit.

Three files live on the device:

| File | Direction | Mutability |
|---|---|---|
| `deck.srs` | host → device | replaced wholesale on every sync |
| `state.bin` | device only | mutated on every grade, never written by sync |
| `revlog.bin` | device → host | append-only, truncated only after confirmed export |

## Card ids

A card id is a 64-bit value derived by the compiler from the deck id plus the card's key field (for Chinese, the headword). It must be stable across recompiles, because it is the only thing joining content to scheduling state.

Consequences worth stating plainly: editing a card's *definition* preserves history, while editing its *headword* creates a new card and orphans the old one. That is the correct behaviour — a different word is a different memory — but the compiler should warn when a sync orphans cards, and the device should garbage-collect state for ids absent from the deck only on explicit request, never silently.

## `deck.srs`

A sectioned container. All integers little-endian, matching the ESP32-S3, so the device can memory-map sections and read them in place without parsing or copying.

```
Header (32 bytes)
  magic          char[8]   "SRSDECK1"
  total_size     u32       whole file, for truncation checks
  section_count  u32
  deck_id        u64       stable across recompiles
  crc32          u32       over everything after the header
```

Followed by `section_count` entries:

```
Section (16 bytes)
  id      char[4]    "META" "CARD" "TEXT" "FONT"
  offset  u32        from start of file
  length  u32
  flags   u32
```

Sections are ordered but independently addressable, so an unknown section id is skipped rather than fatal. That is what lets a newer compiler push to older firmware without bricking a sync.

### `CARD` — fixed-size records

Fixed size so the device can binary-search by id and index by ordinal without building a table in RAM.

```
Card (32 bytes)
  id            u64    stable, sorted ascending across the section
  front_off     u32    byte offset into TEXT
  front_len     u16
  back_off      u32
  back_len      u16
  reading_off   u32    pinyin; 0 when absent
  reading_len   u16
  tags          u16    bitfield, deck-defined
  reserved      u32
```

Records are sorted by `id` so lookup is a binary search over flash, no index needed.

### `TEXT` — UTF-8 blob

Concatenated, not null-terminated; lengths come from the card record. Deduplicated by the compiler, so cards sharing a gloss share bytes.

#### Syllable separators in the reading

For `lang=zh`, the reading field is stored **syllable-separated**: `U+001F`
(ASCII unit separator) between syllables, e.g. `yǒu␟shí␟hou`.

This exists because a diacritic pinyin string does not say where its
syllables end, and the device cannot work it out: a syllable's coda comes
*after* its tone mark, so scanning for tone marks alone splits `yǒushíhou`
as `yǒ | ushí | hou` — the coda gets donated to the next syllable. The
boundaries are already known host-side, in the source TSV's numeric-pinyin
column (`you3shi2hou5`), so the compiler aligns the two columns once (folding
diacritics to base letters; `ü`, `u:` and `v` all fold together) and records
the result. **Alignment failure is a compile error**, never a guessed split:
a wrong split is a wrong reading, and silently shipping one is worse than
refusing to build.

U+001F was chosen because it is a control character: it cannot collide with
reading text, it is skipped by the font subsetter (which has no glyph for
it), and firmware too old to know about it simply falls back to the old
tone-mark heuristic rather than mis-rendering. That is also why the magic
stays `SRSDECK1` — the change is additive and both directions degrade
gracefully.

Readings without a separator (non-`zh` decks, or Chinese input with no
numeric column) are stored verbatim.

### `FONT` — glyph subset

An LVGL binary font containing exactly the glyphs reachable from `TEXT`, plus the Latin range for the UI. Regenerated on every compile.

With no audio to store, flash is abundant and the subset budget should be spent on **size rather than coverage**. Hanzi are stroke-dense and a 2.0" panel is unforgiving, so legibility comes from large glyphs.

Measured, not estimated. The HSK 1+2 deck (347 unique hanzi, 464 glyphs including ASCII) subset from Noto Sans SC at 4bpp:

| Size | Binary |
|---|---|
| 28px (gloss / UI) | 58 KB |
| 48px (headword) | 155 KB |

213KB for both. Extrapolating, a 3,000-hanzi subset at 48px lands near 1.3MB, still trivial against 16MB. Treat font size as free and pick whatever is most readable; the only real constraint is that the subset must be regenerated whenever the deck's glyph set changes.

Generate with `tools/buildfont.sh`, which reads the `.glyphs.txt` the compiler emits.

## `state.bin`

One fixed-size record per card, sorted by id, mirroring `CARD` ordering so both can be walked together.

```
CardState (32 bytes)
  id              u64
  due             i64    unix seconds
  stability       f32    FSRS
  difficulty      f32    FSRS
  last_review     i64    unix seconds; 0 when never reviewed
  reps            u16
  lapses          u16
  state           u8     0 new, 1 learning, 2 review, 3 relearning
  step            u8     index into the learning steps
  reserved        u16
```

Written with a write-ahead pattern: append the new record to a journal, fsync, then update in place. A power cut mid-grade must never corrupt a neighbouring card's state, and this device runs on a battery that can die at any moment.

## `revlog.bin`

Append-only, the source of truth for everything above. Every other number in this document is derivable from it.

```
ReviewEntry (24 bytes)
  card_id    u64
  reviewed   i64    unix seconds, UTC
  rating     u8     1 Again, 2 Hard, 3 Good, 4 Easy
  state      u8     card state before the review
  duration   u16    milliseconds spent on the card
  reserved   u32
```

Never rewritten, only appended and — after the host confirms an export — truncated from the front. Because it is authoritative, a corrupted `state.bin` is recoverable by replaying the log, and exporting it into desktop Anki lets Anki's own FSRS recompute a schedule that agrees with the device's.

Timestamps are UTC. The device's clock comes from NTP over WiFi and is not trustworthy before the first sync, so entries recorded while the clock is unset are marked by a sentinel `reviewed` value and given real times at export, using the elapsed monotonic time since boot.
