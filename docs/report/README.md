# Project report (LaTeX)

The full project report for **CS3530: Computer Networks**: *Fuse: A Per-Stream Reliable Transport Protocol
over UDP — Design, Implementation, Security Hardening and Evaluation of Protocol Version 8*.

| File | Contents |
|---|---|
| `report.tex` | Main file: title page, abstract, contents, and the chapter list |
| `chapters/01-introduction.tex` … `10-conclusion.tex` | The ten chapters |
| `chapters/A-appendices.tex` | Configuration reference, repository guide, build steps, glossary |
| `chapters/references.tex` | Bibliography |
| `report.pdf` | The compiled report |

Chapters: 1 Introduction (problem statement) · 2 Background and related work · 3 Architecture and design
principles · 4 Wire format · 5 Building blocks · 6 Bulk transfer end to end · 7 SDK and multiplexed transfer ·
8 Security model and the version 8 hardening · 9 Testing and evaluation · 10 Limitations, future work and
conclusion.

## Building

```sh
cd docs/report
latexmk -pdf report.tex          # or run pdflatex report.tex three times
```

Needs a TeX distribution with TikZ: `sudo apt install texlive-latex-recommended texlive-latex-extra
texlive-pictures latexmk` (Debian/Ubuntu) or `brew install --cask mactex-no-gui` (macOS).

All measurements in Chapter 9 were taken on one 4-core Linux machine, comparing protocol version 7 (commit
`abbf731`) with version 8 (commit `9706e21`).
