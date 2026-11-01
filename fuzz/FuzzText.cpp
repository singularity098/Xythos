// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// Small parsers and editors that see what the user types or pastes: the text fields' editing (src/ui/TextEdit.*)
// and the colour picker's hex field and colour maths (src/state/Ink.*).

#include "FuzzTarget.hpp"

#include "state/Ink.hpp"
#include "state/Paths.hpp"
#include "ui/TextEdit.hpp"

#include <cmath>
#include <cstring>
#include <vector>

using namespace xythos;

namespace {

// Inside a character (the start of the text counts as a boundary, even before stray continuation bytes)
bool continuation(const std::string& text, size_t pos)
{
    return pos > 0 && pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xc0) == 0x80;
}

// Well-formed UTF-8 (lead bytes followed by the right number of continuation bytes; overlongs aren't checked)
bool validUtf8(const std::string& text)
{
    for (size_t i = 0; i < text.size();)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (n == 0 || i + n > text.size())
            return false;
        for (size_t k = 1; k < n; ++k)
        {
            if (!continuation(text, i + k))
                return false;
        }
        i += n;
    }
    return true;
}

} // namespace

// What's typed or pasted is UTF-8, but a field may start with anything (a settings file edited by hand): any text
// must be safe, and in valid text the cursor and selection stay on character boundaries.
XYTHOS_FUZZ_TARGET(text_edit)
{
    fuzz::Input in(data, size);
    TextEdit edit;
    edit.setText(in.text(32));
    while (!in.empty())
    {
        const bool word = in.flag(), extend = in.flag();
        switch (in.range(0, 12))
        {
        case 0: edit.insert(in.text(8)); break;
        case 1: edit.moveLeft(word, extend); break;
        case 2: edit.moveRight(word, extend); break;
        case 3: edit.moveHome(extend); break;
        case 4: edit.moveEnd(extend); break;
        case 5: edit.backspace(word); break;
        case 6: edit.del(word); break;
        case 7: edit.deleteSelection(); break;
        case 8: edit.selectAll(); break;
        case 9: edit.selectWordAt(static_cast<size_t>(in.range(0, 64))); break;
        case 10: edit.setCursor(static_cast<size_t>(in.range(0, 64)), extend); break;
        case 11: (void)edit.selectedText(); break;
        case 12:
            // the UI changed the text directly, then clamps
            edit.text = in.text(16);
            edit.clamp();
            break;
        }
        FUZZ_CHECK(edit.cursor <= edit.text.size() && edit.anchor <= edit.text.size());
        if (validUtf8(edit.text))
            FUZZ_CHECK(!continuation(edit.text, edit.cursor) && !continuation(edit.text, edit.anchor));
        FUZZ_CHECK(edit.wordLeft(edit.cursor) <= edit.cursor && edit.wordRight(edit.cursor) >= edit.cursor);
        FUZZ_CHECK(edit.charLeft(edit.cursor) <= edit.cursor && edit.charRight(edit.cursor) <= edit.text.size());
    }
    return 0;
}

XYTHOS_FUZZ_TARGET(ink)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (const std::optional<ink::Rgb> color = ink::parseHex(text))
    {
        FUZZ_CHECK(ink::parseHex(ink::toHex(*color)) == color);
        FUZZ_CHECK(ink::isLightEnough(ink::lighten(*color)));
        (void)ink::colorName(*color);
        const ink::Rgb back = ink::fromHsv(ink::toHsv(*color));
        FUZZ_CHECK(std::abs(back.r - color->r) <= 1 && std::abs(back.g - color->g) <= 1 && std::abs(back.b - color->b) <= 1);
    }
    // HSV from any doubles (the wheel's maths with whatever a drag computes)
    if (size >= 24)
    {
        double hsv[3];
        std::memcpy(hsv, data, sizeof hsv);
        (void)ink::colorName(ink::fromHsv({ hsv[0], hsv[1], hsv[2] }));
    }
    // a few colours for the gradient texture
    std::vector<ink::Rgb> colors;
    for (size_t i = 0; i + 3 <= size && colors.size() < 3; i += 3)
        colors.push_back({ data[i], data[i + 1], data[i + 2] });
    if (!colors.empty() && size > 0)
    {
        const auto mode = static_cast<ink::Mode>(data[0] % 3);
        const std::vector<uint8_t> texture = ink::makeTexture(mode, colors);
        FUZZ_CHECK(texture.size() == static_cast<size_t>(ink::kTextureWidth) * ink::kTextureHeight * 4);
        (void)ink::sample(colors, static_cast<double>(data[size - 1]) / 255.0);
    }
    // home folder shortening and expanding: what the Settings folder fields show and take
    const std::string path(text);
    (void)shortenHomePath(path);
    (void)expandHomePath(path);
    return 0;
}
