# Signalsmith Stretch (vendored, with changes)

`signalsmith-stretch.h` is from https://github.com/Signalsmith-Audio/signalsmith-stretch at commit 88061fd
(version 1.4.0 plus a Linear version bump), MIT licensed (`LICENSE.txt`), with Xythos's changes marked
`// Xythos:` in the code; `xythos-changes.patch` is the whole diff from upstream. It needs Signalsmith Linear
(`deps/signalsmith-linear`, a submodule at 0.6.6).

It's vendored rather than a submodule because of those changes (patching a submodule at configure time isn't
reliable on Windows checkouts with CRLF line endings).

The changes make shifted notes land on pitch. Upstream, a steady tone shifted came out off pitch by an amount that
depended on where it fell between FFT bins:

1. `findPeaks()`: each peak's frequency comes from its phase advance over the last interval (exact for a steady
   tone) instead of the energy-weighted centre of the peak's bins.
2. The phase-vocoder prediction interpolates each bin's phase advance between bins before taking out the
   fractional bin's rotation (it interpolated values already rotated by each bin's own centre frequency).
3. The vertical (between-bin) phase prediction also counts the bin's own prediction from its phase advance, so the
   pitch doesn't drift (an octave down came out about 0.4 Hz flat).

Measured by zero crossings on steady sines from 82 to 700 Hz, with the block Xythos uses (120 ms, 30 ms interval,
split computation), worst case: a fifth up 0.28 cents (upstream 12.8), an octave up 0.23 (13.2), an octave down
4.9 (43.8). See the pitch blocks' HQ mode in `docs/Architecture.md`. To update: take the new upstream header and
re-apply the patch.
