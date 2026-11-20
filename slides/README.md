# Presentation slides

A 15-minute, 19-slide Beamer presentation of the project for **CS3530: Computer Networks**: problem statement, design,
the version 8 hardening (both review passes), the measured results, and references.

| File | Contents |
|---|---|
| `presentation.tex` | Beamer source (16:9) |
| `presentation.pdf` | The compiled slides |

## Building

```sh
cd slides
latexmk -pdf presentation.tex        # or: pdflatex presentation.tex && pdflatex presentation.tex
```

Needs a TeX distribution with Beamer and TikZ:
`sudo apt install texlive-latex-recommended texlive-latex-extra texlive-pictures latexmk` (Debian/Ubuntu)
or `brew install --cask mactex-no-gui` (macOS). The `metropolis` theme is used when installed (it ships with
`texlive-latex-extra`); otherwise the slides fall back to the built-in `Madrid` theme.

## Where the numbers come from

All results on the slides were measured on one 4-core Linux machine, comparing version 7 (commit
`abbf731`) with version 8 after both review passes (commit `e5cb73a`): the test counts from `ctest -j8` in four
build configurations, loopback throughput (512 MiB, 4 lanes, median of five runs), and transfers through
`bench/fuse_netem` (32 MiB, 4 lanes, every output checked byte for byte). The project report
(`docs/report/`) has the full tables. The numbered references [1]–[20] are listed on the References slide.
