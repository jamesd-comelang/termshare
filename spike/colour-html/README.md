# Termshare colour spike — VTE HTML export

Question: can VTE's HTML export give termshare coloured cells for
tabs that already exist, without a PTY tee and without restarting
the shell?

## What the spike does

`colour_spike.c` creates a `VteTerminal` with no child process, feeds
it known coloured ANSI with `vte_terminal_feed()` (documented as
interpreting data as if received from a child process), then prints:

1. Full visible text as TEXT and as HTML (`vte_terminal_get_text_format`)
2. Per-row HTML for rows 0–7, with `end_col` 39 and 40, to settle
   whether `end_col` is inclusive or exclusive
3. The same per-row call resolved at runtime with `dlsym()`
4. Row 0 after feeding 5 extra lines, to settle whether range rows
   are visible-relative or absolute (scrollback-inclusive)

Test rows: plain, red (31), bold green (1;32), palette 196
(38;5;196), truecolor (38;2;10;20;30), background red (41),
underline (4), italic (3), red with trailing spaces, wide CJK
characters in yellow, and default-colour resets (39/49).

## Why dlsym matters

The shipping binary is built on Ubuntu 24.04 (VTE 0.76, where
`vte_terminal_get_text_range_format` exists since 0.72) but must run
on Ubuntu 22.04 Jammy (VTE 0.68, where it does not exist). Linking
the symbol directly would make the binary fail to load on Jammy.
Production code must resolve it with `dlsym()` and fall back to the
old attributes API on 0.68. The direct calls in `colour_spike.c`
exist only to compare against the dlsym call; do not copy them
into production code.

## Running

CI: push to `termshare/colour-spike` — workflow
`.github/workflows/colour-spike.yml` builds and runs it under
`xvfb` on ubuntu-24.04 and uploads `colour_spike_output.txt`.

Locally (needs GTK/VTE dev packages, not present in the Muse VM):

```
cc -Wall -Wextra -O2 spike/colour-html/colour_spike.c -o colour_spike \
  $(pkg-config --cflags --libs gtk+-3.0 vte-2.91) -ldl
xvfb-run -a ./colour_spike
```

## Status

Spike branch only. No termshare sharing code is changed here, and
nothing from this directory is merged to master.
