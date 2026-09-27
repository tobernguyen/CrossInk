# Bilingual tooltip (fork of CrossInk)

This fork (`tobernguyen/CrossInk`, branch `bilingual`) adds sentence translation tooltips for
bilingual EPUBs made with the Calibre [Ebook Translator plugin](https://github.com/bookfere/Ebook-Translator-Calibre-Plugin)
("Below original" position). The tooltip is ported from [CrossLingua](https://github.com/ed-fruty/crosslingua-reader).
Everything else is stock CrossInk.

## Behaviour

| Book | Page | Hold a word (1 s) |
|---|---|---|
| Not translated | unchanged | dictionary lookup, as in CrossInk |
| Translated, **Bilingual Tooltip** off | as Calibre made it (both languages) | dictionary lookup |
| Translated, **Bilingual Tooltip** on | original language only | translation of that sentence in a tooltip; tap or Back closes it |

The tooltip sits beside the sentence, on its roomier side (at least 40% of the screen, covering the
sentence if it must). A translation too long for it pages: the box shows `1/3` in its corner, a tap
inside shows the next part (then the first again), and a tap outside or Back closes it.
If a hold finds no translation because the chapter's translations didn't load (out of memory,
file error), a one-line notice says why, with the free heap; the next hold retries.

- A book counts as translated when its first chapters hold at least 3 block elements whose `lang`
  differs from the book's `dc:language`. The scan runs once per book; the result and your choice
  live in `.crosspoint/epub_<id>/bilingual.bin`.
- The toggle is in the reader menu → Settings tab, only on translated books. It defaults to on
  for translated books on touch devices (X4 Pro), off elsewhere.
- Global preferences in the same Settings tab (tap to cycle, saved in `.crosspoint/bilingual-prefs.bin`):
  - **Hold Time** (every book): 0.25 s, 0.5 s (default, the touch SDK's long-press), 0.75 s, 1 s
    (stock CrossInk). One setting for every hold-to-look-up: the tooltip, the dictionary on the
    page, and the lookup inside a definition. Highlighting keeps CrossInk's own 0.5 s hold.
  - **Tooltip Font** (translated books): Interface (Inter 12, default), Interface (Small)
    (Inter 10), or Book Font (the page's font and size, SD-card fonts included).
- Books sent with the CrossPoint Reader Calibre plugin's "Split large chapters/paragraphs"
  optimization work: its ~7 KB file splits can fall between a paragraph and its translation, so the
  tooltip reads on into the next file for it, and it joins paragraphs split into several `<p>`s.
- Keep the plugin's "Set target language code to language metadata" **off**: the book's language
  must stay the original's, or the translations can't be told apart.
- OTA ("Check for updates") installs upstream CrossInk and replaces this fork. Update the fork instead.

## Where the code is

All logic is new files in `src/bilingual/`, with host tests in `test/bilingual/` (`BilingualTooltipTest` runs
the tooltip on small sample chapters against stub page/renderer/storage headers in `tooltip_stubs/`). The changes to upstream
files are small and marked `// bilingual`:

| File | Change |
|---|---|
| `lib/Epub/Epub/parsers/ElementSkipHook.h` | new: hook the layout parser calls to hide an element |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` | include + skip hooked elements (next to `display:none`) |
| `src/activities/reader/EpubReaderActivity.cpp` | book open/close, hold → tooltip (own hold time), tap/Back closes, draw + font prewarm scan, toggle action |
| `src/activities/reader/EpubReaderMenuModel.h` | menu action + 3 drawer rows + availability flag; tab row limit 12 → 16 |
| `src/activities/reader/EpubReaderTouchMenuActivity.cpp` | the toggle row and the two preference rows |
| `src/activities/reader/EpubGrayscale.h` / `.cpp` | `grayscaleOverlay` hook, called after the page text in each text-grayscale plane (tooltip anti-aliasing) |
| `src/activities/reader/DictionaryDefinitionActivity.cpp` | in-definition lookup uses the shared Hold Time |
| `lib/I18n/translations/english.yaml` | `STR_BILINGUAL_TOOLTIP`, `STR_TOOLTIP_*` (other languages fall back to English) |
| `test/CMakeLists.txt` | `add_subdirectory(bilingual)` |
| `test/reader_drawer_model/ReaderDrawerModelTest.cpp` | Settings tab expectations include Hold Time (CI runs this suite too) |
| `.github/workflows/bilingual.yml` | new: fork build |

No cache format changes: a book's layout is rebuilt when the toggle changes, and the tooltip finds
the page's stretch of the chapter by content instead of stored paragraph indices.

## Updating from upstream CrossInk

`main` mirrors `uxjulia/CrossInk` and is never committed to; the fork's work is the `bilingual` branch on top.

```sh
git fetch upstream
git switch main && git merge --ff-only upstream/main && git push origin main
git switch bilingual && git rebase main
# resolve conflicts (only ever in the `// bilingual` spots above), then:
git push --force-with-lease origin bilingual
```

The push runs `.github/workflows/bilingual.yml`: host tests, the X4 Pro build, and a new
`bilingual-latest` pre-release with `firmware-x4-pro-bilingual.bin`. Copy it to the SD card and
install it from Settings → System → SD Card Firmware Update.

## Maintenance notes

- Keep the fork as **one commit** on top of upstream (fold changes in with `git commit --amend` or a
  squash before updating), so a rebase resolves each conflict once. `git rerere` is on in the local
  clone, so a resolution repeated across releases is replayed automatically.
- The fork's additions sit next to related upstream entries rather than at the end of lists, so
  upstream appending strings, menu entries or test suites doesn't collide with them.
- The one line that conflicts on most releases is the drawer's per-tab row limit
  (`ReaderDrawerTabCatalog::items` in `EpubReaderMenuModel.h`): take upstream's number + 4.
  `BilingualMenu.ForkRowsFitTheSettingsTab` fails to compile if it's too small for every row.
- Don't use GitHub's "Sync fork" button on `bilingual`: it merges instead of rebasing.

## Building locally

```sh
pio run -e x4-pro                                  # .pio/build/x4-pro/firmware-x4-pro-v*.bin
cmake -S test -B build/test && cmake --build build/test --target BilingualTest BilingualTooltipTest ReaderDrawerModelTest
build/test/bilingual/BilingualTest && build/test/bilingual/BilingualTooltipTest && build/test/reader_drawer_model/ReaderDrawerModelTest
```

## Known limits

- Touch only (long-press); no button stepping between sentences.
- CrossInk pages don't mark paragraph starts; the tooltip finds them from the extra space between
  lines (paragraph spacing, heading margins). With paragraph spacing off, a heading with no
  margin and no full stop still merges into the sentence after it.
- Books are scanned in their first 12 spine items; a book whose translations start later isn't
  detected.
