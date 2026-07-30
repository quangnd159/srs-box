# HSK 3.0 vocabulary source notes

## Source

Word list and level assignment: [elkmovie/hsk30](https://github.com/elkmovie/hsk30),
`wordlist.txt`, commit as fetched 2026-07-30 (`master` branch, MIT-licensed
extraction of the wordlist from the official 2021 HSK 3.0 standard document,
`(c) 2021 Pleco Inc`, OCR'ed from the PDF published by 中国教育部/国家语委
in *国际中文教育中文水平等级标准*). Raw per-level counts from the file matched
the official standard exactly before any de-duplication:

| Level | Official count | elkmovie/hsk30 raw count |
|---|---|---|
| 1 | 500 | 500 |
| 2 | 772 | 772 |
| 3 | 973 | 973 |
| 4 | 1000 | 1000 |
| 5 | 1071 | 1071 |
| 6 | 1140 | 1140 |

Pinyin, traditional forms, and English glosses: [drkameleon/complete-hsk-vocabulary](https://github.com/drkameleon/complete-hsk-vocabulary),
`complete.json`, `main` branch as fetched 2026-07-30 (a CC-CEDICT-derived
per-character/word database with pronunciation, meanings, and traditional
forms). Its own HSK 3.0 (`newest-N`) level tags were **not** used — they
disagree substantially with the official counts above (e.g. it tags only 294
words `newest-1` vs the official 500) — the file was used purely as a
simplified-word → {pinyin, traditional, gloss} dictionary, keyed by the
elkmovie wordlist.

## Output

`decks/hsk30-l1.src.tsv` … `decks/hsk30-l6.src.tsv`, 5 tab-separated columns:
`simplified  traditional  numeric-pinyin  tone-marked-pinyin  english-draft-gloss`,
one word per line, in official list order within each level.

Final counts after de-duplication (see below):

| Level | Final count |
|---|---|
| 1 | 497 |
| 2 | 747 |
| 3 | 949 |
| 4 | 973 |
| 5 | 1057 |
| 6 | 1119 |
| **Total** | **5342** (of 5456 official) |

## Cleanup applied to the raw wordlist

- **Annotation stripping.** The source marks disambiguated senses like
  `白（形）`, `爸爸｜爸`, `称¹（动）`. Parenthetical POS/example annotations
  were stripped; `A｜B` alternate-form notation kept the first (fuller) form
  (e.g. `爸爸｜爸` → `爸爸`, matching how `decks/hsk1-2.raw.tsv` already lists
  `爸爸`). Superscript sense numbers (`称¹`/`称²`) were stripped to plain text.
- **Within-level duplicates (33 dropped, merged into a single row each).**
  A few characters are listed twice within the same level to represent two
  senses/readings (e.g. `长` cháng "long" vs zhǎng "chief"; `面¹`/`面²` "face"
  vs "noodles"). These were merged into one row per level, following the
  existing convention in `decks/hsk1-2.raw.tsv` (see its `长` entry): multiple
  pinyin readings are comma-joined in both pinyin columns, and the
  corresponding glosses are joined with ` | `. Affected words: L1 `地 干`;
  L2 `长 倒 得 面 省 实在 头` (`老` and `背` and `为` turned out to already
  exist at a lower level — see next point — so they were not merged as
  duplicates, just resolved at that lower level); L3 `把 初 调 精神 任`;
  L4 `倒车 划 卷 批 挑`; L5 `编辑 品 扇 吐`; L6 `副 界 料 露 蒙 族`.
- **Cross-level duplicates (81 occurrences dropped).** 78 distinct simplified
  forms appear at two (occasionally three) different levels, generally
  because the same character is re-taught at a higher level for a secondary
  sense (e.g. `好`, `花` at L1/L2/L4; `称`, `封` at L2/L5). Per the task's
  instruction, each was kept only at its lowest level and dropped from every
  higher one. Three of the within-level-merge candidates above (`老`, `背`,
  `为`) turned out to *also* be cross-level duplicates with an even lower
  level, so their combined-sense gloss was applied at that lower level
  instead of the level where the pair was found.
- **Two pattern/placeholder entries** from the source (`…极了`, at L3, and
  `…分之…`, at L4) use a leading ellipsis to mark a slot for an adjective or
  number (e.g. 好极了, 三分之一). Both were missing from the CC-CEDICT-derived
  dictionary under that literal string, so pinyin/gloss were filled by hand
  and the ellipsis was dropped from the simplified/traditional columns
  (`极了`, `分之`), since the ellipsis isn't part of the lexical item and
  isn't a real hanzi the font subsetter needs to render.
- **Total reduction accounting**: 5456 official − 33 (within-level merges,
  which remove one row per merge) − 81 (cross-level drops) = 5342, matching
  the sum of the six output files.

## Pinyin/gloss lookup and hand fixes

For each cleaned simplified form, the compiler looked up
`complete.json` by exact simplified-string match; only 2 of 5342 words were
missing (the two pattern words above). 879 words had more than one
pronunciation/sense entry in the dictionary (heteronyms, or CEDICT splitting
senses across multiple "forms"); an automatic picker filtered out
surname-only, archaic-only, "variant of X"-only, and proper-noun-pinyin
senses when a better alternative existed, then chose the remaining sense
with the richest meanings list.

That heuristic is not perfect — CC-CEDICT lists sub-entries by tone number,
not frequency, so neutral-tone grammatical particles and some common
heteronyms occasionally lost to a rarer tone-1 reading with more listed
glosses. The following ~20 words were caught by manual review and
hand-corrected (documented here since they'll otherwise look like silent
errors): `吧`, `吗`, `了`, `西`, `没`, `年`, `子`, `着` (L1, corrected to their
common toneless/particle readings `ba`, `ma`, `le`, `xī`, `méi`, `nián`,
`zi`, `zhe`); `啊`, `数`, `克`, `卡` (L2, corrected to `a`, `shù`, `kè`,
`kǎ`); `生意` (L3, `shēngyi` "business", not the literary "life force"
sense); `圈` (L4, `quān` "circle", not `juān` "to pen in"); `价`, `吓`,
`闲` (L5, `jià` "price", `xià` "to scare", `xián` "idle"); `嘛`, `炮` (L6,
`ma` modal particle, `pào` "cannon"). A few similarly capitalized
proper-noun senses (`保`, `和平`, `东北`, `东方`, `西北`, `网络`, `大陆`) were
fixed automatically once the picker was taught to prefer a lowercase-pinyin
sense over a capitalized one when both exist.

Given the volume (5342 words), the remaining automatic picks were **not**
individually verified against a native-speaker reference and should be
treated as a draft, consistent with the brief — they will be replaced by
human-quality Vietnamese translations later, and any remaining wrong-sense
picks are far more likely in the less pedagogically load-bearing L4-L6 lists
than in L1-L3, which got closer manual attention.

## Pinyin formatting note

`decks/hsk1-2.raw.tsv` sometimes splits multi-syllable numeric/tone pinyin
with a space at word-internal grammatical boundaries (e.g. `打电话` →
`da3 dian4hua4`, splitting the verb 打 from the object 电话) while joining
the rest into one token. Reproducing that per-word morpheme segmentation
automatically was out of scope for this pass; instead, all syllables of a
given entry are joined into a single space-free token in both the numeric
and tone-marked pinyin columns (e.g. `da3dian4hua4` / `dǎdiànhuà`). This is
a deliberate simplification, not a data error, and does not affect
diacritic correctness.

## Traditional-form notes

A handful of merged within-level entries cover senses whose traditional
forms differ (a side effect of simplified Chinese collapsing multiple
traditional characters into one), and both are given comma-joined:
`干` → `乾, 幹` (gān "dry" / gàn "to do"); `面` → `面, 麵` (miàn "face" /
"noodles"); `划` → `划, 劃` (huá "to row" / huà "to plan"); `卷` → `捲, 卷`
(juǎn "to roll" / juàn "volume").
