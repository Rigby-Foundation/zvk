bcdec is vendored from https://github.com/iOrange/bcdec
at commit `80859ed3b7afb1c527a2a99d70c61457bea72d0c`.

Upstream SHA256 (bcdec.h): `134520764d96f70a27db814173616c89ad85db95fbd3328417c7a8c77e7ca189`.

zVk selects its MIT license. This is a block-format decoder; rendering and
shader execution remain on the GPU. Upstream notices are retained in the header.

`portability.patch` makes signed BC4/BC5 endpoints independent of plain-char
signedness and removes undefined signed shifts in BC6H sign extension and
half-to-float conversion. No sanitizer diagnostics are suppressed.

Patched SHA256: `adc83cc574e9a98436914e650594f7025d21179b7ed5e2a500efe7062d45b49a`.
