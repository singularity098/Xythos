// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "FileLooks.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace xythos {

namespace {

// Random numbers from the key: FNV-1a for the seed, then splitmix64. Both are fixed here (not std::hash), so a file
// looks the same in every build and every version.
class Dice {
public:
    explicit Dice(std::string_view key)
    {
        for (const char c : key)
        {
            fState ^= static_cast<unsigned char>(c);
            fState *= 0x100000001b3ull;
        }
    }

    uint64_t next()
    {
        uint64_t z = (fState += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    int below(int n) { return static_cast<int>(next() % static_cast<uint64_t>(n)); }
    float unit() { return static_cast<float>(next() >> 40) / static_cast<float>(1ull << 24); } // 0..1
    float between(float lo, float hi) { return lo + (hi - lo) * unit(); }

private:
    uint64_t fState = 0xcbf29ce484222325ull;
};

} // namespace

AmpLook ampLook(std::string_view key)
{
    AmpLook look;
    if (key.empty())
        return look;
    Dice dice(key);
    look.knobs = 3 + dice.below(5);
    look.grille = static_cast<AmpLook::Grille>(dice.below(AmpLook::kGrilles));
    look.badge = static_cast<Badge>(dice.below(kBadges));
    dice.next(); // was the body (head or combo); still drawn, so the other choices stay as they were
    // reds mostly, oranges now and then
    const float u = dice.unit();
    look.hue = AmpLook::kMinHue + (AmpLook::kMaxHue - AmpLook::kMinHue) * u * u;
    for (float& turn : look.turns)
        turn = dice.between(0.2f, 0.8f);
    return look;
}

CabLook cabLook(std::string_view key)
{
    CabLook look;
    if (key.empty())
        return look;
    Dice dice(key);
    look.cone = static_cast<CabLook::Cone>(dice.below(CabLook::kCones));
    look.cloth = static_cast<CabLook::Cloth>(dice.below(CabLook::kCloths));
    look.badge = static_cast<Badge>(dice.below(kBadges));
    look.corners = dice.below(2) == 1;
    look.hue = dice.between(-60.0f, 60.0f);
    return look;
}

std::string_view cabImageSize(std::string_view id)
{
    const size_t dash = id.rfind('-');
    return dash == std::string_view::npos ? std::string_view() : id.substr(0, dash);
}

std::vector<std::string> cabImagesOfSize(const std::vector<std::string>& ids, std::string_view size)
{
    std::vector<std::string> result;
    for (const std::string& id : ids)
        if (cabImageSize(id) == size)
            result.push_back(id);
    const auto number = [](const std::string& id) { return std::atoi(id.c_str() + id.rfind('-') + 1); };
    std::stable_sort(result.begin(), result.end(),
                     [&number](const std::string& a, const std::string& b) { return number(a) < number(b); });
    return result;
}

std::string_view cabSizeInName(std::string_view name)
{
    const auto digit = [&name](size_t i) { return i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])); };
    for (size_t i = 0; i + 4 <= name.size(); ++i)
    {
        // "<count>x<inches>", not part of a longer number
        if ((i > 0 && digit(i - 1)) || digit(i + 4) || (name[i + 1] != 'x' && name[i + 1] != 'X'))
            continue;
        for (const char* size : kCabSizes)
            if (name[i] == size[0] && name[i + 2] == size[2] && name[i + 3] == size[3])
                return size;
    }
    return {};
}

std::string defaultCabImage(std::string_view key, std::string_view fileName, const std::vector<std::string>& ids)
{
    if (ids.empty())
        return {};
    if (key.empty())
    {
        const std::vector<std::string> fours = cabImagesOfSize(ids, "4x12");
        return fours.empty() ? ids.front() : fours.front();
    }
    // most IRs are of a 4x12, so the rest only when the name says so (owner, 2026-10-29; was any size)
    std::vector<std::string> pool;
    if (const std::string_view size = cabSizeInName(fileName); !size.empty())
        pool = cabImagesOfSize(ids, size);
    if (pool.empty())
        pool = cabImagesOfSize(ids, "4x12");
    if (pool.empty())
        pool = ids;
    Dice dice(key);
    return pool[static_cast<size_t>(dice.below(static_cast<int>(pool.size())))];
}

std::string defaultPedalImage(std::string_view key, const std::vector<std::string>& ids)
{
    if (ids.empty())
        return {};
    // in their numbers' order, so a new image (a higher number) only changes some files' picks, never the order
    std::vector<std::string> sorted = ids;
    const auto number = [](const std::string& id) { return std::atoi(id.c_str() + id.rfind('-') + 1); };
    std::stable_sort(sorted.begin(), sorted.end(),
                     [&number](const std::string& a, const std::string& b) { return number(a) < number(b); });
    if (key.empty())
        return sorted.front();
    Dice dice(key);
    return sorted[static_cast<size_t>(dice.below(static_cast<int>(sorted.size())))];
}

namespace {

// Picked by eye from each photo (owner, 2026-10-31: the heavy, very distorted tones get the strangest-looking heads)
struct AmpImageStyle {
    const char* id;
    unsigned style;
};
constexpr AmpImageStyle kAmpImageStyles[] = {
    { "amp-1", kAmpHighGain },
    { "amp-2", kAmpClean },
    { "amp-3", kAmpHighGain },
    { "amp-4", kAmpClean },
    { "amp-5", kAmpExtreme },
    { "amp-6", kAmpCrunch },
    { "amp-7", kAmpCrunch },
    { "amp-8", kAmpHighGain },
    { "amp-9", kAmpClean | kAmpJazz },
    { "amp-10", kAmpExtreme },
    { "amp-11", kAmpClean | kAmpJazz },
    { "amp-12", kAmpExtreme },
    { "amp-13", kAmpExtreme },
    { "amp-14", kAmpClean },
    { "amp-15", kAmpCrunch },
    { "amp-16", kAmpCrunch | kAmpExtreme },
    { "amp-17", kAmpHighGain },
    { "amp-18", kAmpExtreme },
    { "amp-19", kAmpExtreme },
    { "amp-20", kAmpCrunch },
    { "amp-21", kAmpExtreme },
    { "amp-22", kAmpClean | kAmpCrunch },
    { "amp-23", kAmpCrunch | kAmpHighGain },
    { "amp-24", kAmpHighGain },
    { "amp-25", kAmpHighGain },
    { "amp-26", kAmpCrunch },
    { "amp-27", kAmpHighGain | kAmpExtreme },
    { "amp-28", kAmpExtreme },
    { "amp-29", kAmpHighGain | kAmpExtreme },
    { "amp-30", kAmpClean | kAmpCrunch },
    { "amp-31", kAmpClean | kAmpCrunch },
    { "amp-32", kAmpCrunch | kAmpHighGain },
    { "amp-33", kAmpClean | kAmpCrunch },
    { "amp-34", kAmpClean | kAmpJazz },
    { "amp-35", kAmpHighGain },
    { "amp-36", kAmpHighGain },
    { "amp-37", kAmpClean | kAmpCrunch },
    { "amp-38", kAmpHighGain | kAmpExtreme },
    { "amp-39", kAmpHighGain },
    { "amp-40", kAmpHighGain },
    { "amp-41", kAmpClean | kAmpCrunch },
    { "amp-42", kAmpExtreme },
    { "amp-43", kAmpClean | kAmpJazz },
    { "amp-44", kAmpExtreme },
    { "amp-45", kAmpExtreme },
    { "amp-46", kAmpExtreme },
    { "amp-47", kAmpCrunch | kAmpHighGain },
    { "amp-48", kAmpHighGain },
    { "amp-49", kAmpHighGain },
    { "amp-50", kAmpClean | kAmpCrunch },
    { "amp-51", kAmpCrunch | kAmpHighGain },
    { "amp-52", kAmpClean | kAmpCrunch },
    { "amp-53", kAmpCrunch },
    { "amp-54", kAmpHighGain | kAmpExtreme },
    { "amp-55", kAmpHighGain },
    { "amp-56", kAmpExtreme },
    { "amp-57", kAmpCrunch },
    { "amp-58", kAmpHighGain | kAmpExtreme },
    { "amp-59", kAmpCrunch },
    { "amp-60", kAmpCrunch | kAmpHighGain },
    { "amp-61", kAmpClean | kAmpCrunch },
    { "amp-62", kAmpHighGain | kAmpExtreme },
    { "amp-63", kAmpClean | kAmpJazz },
    { "amp-64", kAmpHighGain },
    { "amp-65", kAmpCrunch },
    { "amp-66", kAmpClean },
    { "amp-67", kAmpExtreme },
    { "amp-68", kAmpHighGain },
    { "amp-69", kAmpCrunch },
    { "amp-70", kAmpHighGain },
    { "amp-71", kAmpCrunch | kAmpHighGain },
    { "amp-72", kAmpExtreme },
    { "amp-73", kAmpHighGain | kAmpExtreme },
    { "amp-74", kAmpHighGain | kAmpExtreme },
    { "amp-75", kAmpExtreme },
    { "amp-76", kAmpCrunch | kAmpHighGain },
    { "amp-77", kAmpClean | kAmpJazz },
    { "amp-78", kAmpHighGain | kAmpExtreme },
};

std::string lowerAscii(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool isAlpha(char c) { return c >= 'a' && c <= 'z'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Whether `word` is in `text` (lowercase) and not inside a longer word: "lead" in "Lead 2" and "lead_ch" but not in
// "pleads"; with `prefix` it may go on ("metal" in "metallica")
bool hasWord(const std::string& text, std::string_view word, bool prefix = false)
{
    for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
    {
        const size_t end = at + word.size();
        if ((at == 0 || !isAlpha(text[at - 1])) && (prefix || end >= text.size() || !isAlpha(text[end])))
            return true;
    }
    return false;
}

// A gain setting in the name: "gain 7", "gain7", "g7", "gain-10", "g 8.5"; -1 if none
double gainInName(const std::string& text)
{
    for (const std::string_view word : { std::string_view("gain"), std::string_view("g") })
    {
        for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
        {
            if (at > 0 && isAlpha(text[at - 1]))
                continue;
            size_t i = at + word.size();
            while (i < text.size() && (text[i] == ' ' || text[i] == '_' || text[i] == '-' || text[i] == '@'))
                ++i;
            if (i >= text.size() || !isDigit(text[i]))
                continue;
            double value = 0.0;
            while (i < text.size() && isDigit(text[i]))
                value = value * 10.0 + (text[i++] - '0');
            if (i + 1 < text.size() && (text[i] == '.' || text[i] == ',') && isDigit(text[i + 1]))
                value += (text[i + 1] - '0') / 10.0;
            if (value <= 10.0)
                return value;
        }
    }
    return -1.0;
}

} // namespace

unsigned ampImageStyle(std::string_view id)
{
    for (const AmpImageStyle& entry : kAmpImageStyles)
        if (id == entry.id)
            return entry.style;
    return 0;
}

unsigned ampStyleInName(std::string_view name)
{
    const std::string text = lowerAscii(name);
    const auto any = [&text](std::initializer_list<std::string_view> words, bool prefix = false) {
        for (const std::string_view word : words)
            if (hasWord(text, word, prefix))
                return true;
        return false;
    };
    // what the name says about the sound wins over the amp it names: a "Clean" channel of a metal amp is clean, a
    // "Crunch" setting of a Plexi is crunch
    if (any({ "jazz", "warm", "mellow", "smooth" }))
        return kAmpJazz;
    if (any({ "clean", "cln", "glass", "glassy", "sparkle", "chime", "pristine", "acoustic" }))
        return kAmpClean;
    if (any({ "djent", "brutal", "doom", "sludge", "grind", "death", "thall", "extreme", "insane", "evil", "chug",
              "fuzz" }))
        return kAmpExtreme;
    if (any({ "heavy", "lead", "high gain", "highgain", "hi gain", "higain", "hg", "shred", "thrash", "dist",
              "distortion", "hot" })
        || any({ "metal" }, true))
        return kAmpHighGain;
    if (any({ "crunch", "crnch", "edge", "breakup", "break up", "blues", "bluesy", "drive", "overdrive", "od", "rock",
              "classic" }))
        return kAmpCrunch;
    // then the amp it names
    if (any({ "mesa", "rectifier", "recto", "5150", "6505", "uberschall", "fortin", "boogie" }))
        return kAmpExtreme;
    if (any({ "jcm", "plexi", "evh", "slo", "soldano", "friedman", "jvm" }, true))
        return kAmpHighGain;
    if (any({ "vox", "ac30", "ac15", "marshall", "dumble", "orange" }))
        return kAmpCrunch;
    if (any({ "jc", "jc120" }))
        return kAmpJazz;
    if (any({ "twin", "deluxe", "fender", "princeton", "bassman" }))
        return kAmpClean;
    // a gain number: a fuzzy guess at the rest
    if (const double gain = gainInName(text); gain >= 0.0)
        return gain < 3.0 ? kAmpClean : gain < 6.0 ? kAmpCrunch : gain < 9.0 ? kAmpHighGain : kAmpExtreme;
    return 0;
}

std::string defaultAmpImage(std::string_view key, std::string_view fileName, const std::vector<std::string>& ids)
{
    // the heads only (the half stacks are in the same folder), in their numbers' order, so the pick doesn't depend
    // on the order the ids come in
    const std::vector<std::string> sorted = cabImagesOfSize(ids, "amp");
    if (sorted.empty())
        return {};
    if (key.empty())
        return sorted.front();
    std::vector<std::string> pool;
    if (const unsigned style = ampStyleInName(fileName); style != 0)
        for (const std::string& id : sorted)
            if (ampImageStyle(id) & style)
                pool.push_back(id);
    if (pool.empty())
        pool = sorted;
    Dice dice(key);
    return pool[static_cast<size_t>(dice.below(static_cast<int>(pool.size())))];
}

std::string defaultStackImage(std::string_view key, const std::vector<std::string>& ids)
{
    const std::vector<std::string> sorted = cabImagesOfSize(ids, "stack");
    if (sorted.empty())
        return {};
    if (key.empty())
        return sorted.front();
    Dice dice(key);
    return sorted[static_cast<size_t>(dice.below(static_cast<int>(sorted.size())))];
}

bool comboInName(std::string_view name)
{
    return hasWord(lowerAscii(name), "combo", true);
}

} // namespace xythos
