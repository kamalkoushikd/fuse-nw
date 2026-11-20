# Presentation slides

A 15-minute, 20-slide presentation of the project for **CS3530: Computer Networks** (17 content slides
including the title and demo, 2 reference slides, 1 appendix slide). Plain Beamer, with JetBrains Mono as the
monospace font.

| File | Contents |
|---|---|
| `presentation.tex` | Beamer source (16:9) |
| `results.tex` | The measured numbers used on the results slides, with where they come from |
| `fonts/` | JetBrains Mono (Apache License 2.0, see `fonts/NOTICE.txt`), bundled so the slides build the same everywhere |
| `presentation.pdf` | The compiled slides |

## Building

The slides use `fontspec`, so they need **LuaLaTeX**:

```sh
cd slides
latexmk -lualatex presentation.tex        # or: lualatex presentation.tex (twice)
```

Debian/Ubuntu: `sudo apt install texlive-luatex texlive-latex-extra texlive-pictures fonts-lmodern latexmk`.
macOS: `brew install --cask mactex-no-gui`.

## Where the numbers come from

- **Fuse vs QUIC** (throughput, latency under load, the network-impairment table): `bench/RESULTS.md`,
  measured on a 16-core host at protocol versions 5–7 against quinn.
- **Fuse vs TCP vs raw UDP** (throughput, latency, delivered fraction under loss): measured for this presentation
  on a 4-core Linux host at protocol version 8 over loopback (512 MiB in memory, median of 3 runs; 64-byte probes
  at 2000/s for 5 s with 4 bulk streams; 32 MiB through `bench/fuse_netem` at 2 % and 5 % loss). TCP could not be
  tested under loss on that host because its kernel has no `netem`.
