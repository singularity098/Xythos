// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098
// Prints the face glyphs of the given block types (src/ui/BlockGlyphs.cpp) as JSON, for tools/blender/render_styles.py:
//   g++ -std=c++20 -Isrc -Ideps/NeuralAmpModelerCore/Dependencies tools/blender/dump_glyphs.cpp src/ui/BlockGlyphs.cpp -o dump_glyphs
//   ./dump_glyphs overdrive delay > glyphs.json
#include "ui/BlockGlyphs.hpp"
#include <cstdio>
using namespace xythos;
int main(int argc, char** argv) {
  printf("{");
  for (int i = 1; i < argc; ++i) {
    const Glyph* g = blockGlyph(argv[i]);
    if (!g) continue;
    printf("%s\"%s\":{\"strokes\":[", i > 1 ? "," : "", argv[i]);
    for (size_t s = 0; s < g->strokes.size(); ++s) {
      printf("%s[", s ? "," : "");
      for (size_t p = 0; p < g->strokes[s].size(); ++p) printf("%s[%.2f,%.2f]", p ? "," : "", g->strokes[s][p].first, g->strokes[s][p].second);
      printf("]");
    }
    printf("],\"dots\":[");
    for (size_t d = 0; d < g->dots.size(); ++d) printf("%s[%.2f,%.2f,%.2f]", d ? "," : "", g->dots[d].x, g->dots[d].y, g->dots[d].r);
    printf("]}");
  }
  printf("}\n");
}
