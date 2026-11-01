// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "state/AudioFile.hpp"

#include "state/Paths.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace xythos {

namespace {

void put16(std::string& out, uint32_t v)
{
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
}

void put32(std::string& out, uint32_t v)
{
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

uint32_t get16(std::string_view b, size_t at)
{
    return static_cast<uint8_t>(b[at]) | static_cast<uint32_t>(static_cast<uint8_t>(b[at + 1])) << 8;
}

uint32_t get32(std::string_view b, size_t at) { return get16(b, at) | get16(b, at + 2) << 16; }

// ------------------------------------------------------------------------------------------------------------------
// FLAC

// Writes bits most significant first
class BitWriter {
public:
    void bits(uint64_t value, int count)
    {
        for (int i = count - 1; i >= 0; --i)
        {
            fByte = static_cast<uint8_t>((fByte << 1) | ((value >> i) & 1));
            if (++fFill == 8)
            {
                fOut.push_back(fByte);
                fByte = 0;
                fFill = 0;
            }
        }
    }

    void signedBits(int64_t value, int count) { bits(static_cast<uint64_t>(value) & ((uint64_t { 1 } << count) - 1), count); }

    void unary(uint64_t zeros)
    {
        // zeros, then a one; long runs a byte at a time
        while (zeros >= 8 && fFill == 0)
        {
            fOut.push_back(0);
            zeros -= 8;
        }
        for (; zeros > 0; --zeros)
            bits(0, 1);
        bits(1, 1);
    }

    void align()
    {
        if (fFill > 0)
            bits(0, 8 - fFill);
    }

    const std::vector<uint8_t>& bytes() const { return fOut; }
    size_t bitCount() const { return fOut.size() * 8 + static_cast<size_t>(fFill); }

private:
    std::vector<uint8_t> fOut;
    uint8_t fByte = 0;
    int fFill = 0;
};

uint8_t crc8(const uint8_t* data, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = static_cast<uint8_t>(crc & 0x80 ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

uint16_t crc16(const uint8_t* data, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i)
    {
        crc ^= static_cast<uint16_t>(data[i] << 8);
        for (int b = 0; b < 8; ++b)
            crc = static_cast<uint16_t>(crc & 0x8000 ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}

constexpr int kBlockSize = 4096;
constexpr int kMaxPartitionOrder = 8;

// The residual of FLAC's fixed predictor of `order` (0-4) for samples [order, n)
void fixedResidual(const int64_t* x, int n, int order, int64_t* out)
{
    for (int i = order; i < n; ++i)
    {
        switch (order)
        {
        case 0: out[i] = x[i]; break;
        case 1: out[i] = x[i] - x[i - 1]; break;
        case 2: out[i] = x[i] - 2 * x[i - 1] + x[i - 2]; break;
        case 3: out[i] = x[i] - 3 * x[i - 1] + 3 * x[i - 2] - x[i - 3]; break;
        default: out[i] = x[i] - 4 * x[i - 1] + 6 * x[i - 2] - 4 * x[i - 3] + x[i - 4]; break;
        }
    }
}

uint64_t zigzag(int64_t v) { return v >= 0 ? static_cast<uint64_t>(v) << 1 : (static_cast<uint64_t>(-(v + 1)) << 1) | 1; }

// Bits to Rice-code `values` with parameter k
uint64_t riceBits(const uint64_t* values, int n, int k)
{
    uint64_t total = 0;
    for (int i = 0; i < n; ++i)
        total += (values[i] >> k) + 1 + static_cast<uint64_t>(k);
    return total;
}

// The best Rice parameter (0-14) for `values`, and its cost in bits
std::pair<int, uint64_t> bestRice(const uint64_t* values, int n)
{
    if (n == 0)
        return { 0, 0 };
    uint64_t sum = 0;
    for (int i = 0; i < n; ++i)
        sum += values[i];
    const double mean = static_cast<double>(sum) / n;
    int guess = mean > 1.0 ? static_cast<int>(std::floor(std::log2(mean))) : 0;
    guess = std::clamp(guess, 0, 14);
    int best = guess;
    uint64_t bestCost = riceBits(values, n, guess);
    for (int k : { guess - 1, guess + 1 })
    {
        if (k < 0 || k > 14)
            continue;
        const uint64_t cost = riceBits(values, n, k);
        if (cost < bestCost)
        {
            best = k;
            bestCost = cost;
        }
    }
    return { best, bestCost };
}

// A residual's partitioning: its order and each partition's Rice parameter, and the total cost in bits
struct RicePlan {
    int partitionOrder = 0;
    std::array<int, 1 << kMaxPartitionOrder> params {};
    uint64_t bits = std::numeric_limits<uint64_t>::max();
};

RicePlan planRice(const uint64_t* u, int n, int order)
{
    RicePlan best;
    for (int p = 0; p <= kMaxPartitionOrder; ++p)
    {
        if (n % (1 << p) != 0 || (n >> p) <= order)
            break;
        RicePlan plan;
        plan.partitionOrder = p;
        plan.bits = 2 + 4; // coding method, partition order
        const int parts = 1 << p, size = n >> p;
        for (int k = 0; k < parts; ++k)
        {
            const int start = k == 0 ? order : k * size;
            const int end = (k + 1) * size;
            const auto [param, cost] = bestRice(u + start, end - start);
            plan.params[static_cast<size_t>(k)] = param;
            plan.bits += 4 + cost;
        }
        if (plan.bits < best.bits)
            best = plan;
    }
    return best;
}

// One channel of a block as a subframe: constant if it is, else the cheapest fixed predictor (or verbatim)
struct Subframe {
    enum Kind { Constant, Verbatim, Fixed } kind = Verbatim;
    int order = 0;
    RicePlan rice;
    uint64_t bits = 0;
};

Subframe planSubframe(const int64_t* x, int n, int bps, std::vector<int64_t>& residual, std::vector<uint64_t>& u)
{
    Subframe best;
    best.kind = Subframe::Verbatim;
    best.bits = 8 + static_cast<uint64_t>(n) * bps;
    if (std::all_of(x, x + n, [&](int64_t v) { return v == x[0]; }))
    {
        best.kind = Subframe::Constant;
        best.bits = 8 + static_cast<uint64_t>(bps);
        return best;
    }
    for (int order = 0; order <= 4 && order < n; ++order)
    {
        fixedResidual(x, n, order, residual.data());
        bool fits = true;
        for (int i = order; i < n; ++i)
        {
            if (residual[i] > std::numeric_limits<int32_t>::max() || residual[i] < std::numeric_limits<int32_t>::min())
                fits = false;
            u[i] = zigzag(residual[i]);
        }
        if (!fits)
            continue;
        RicePlan plan = planRice(u.data(), n, order);
        const uint64_t bits = 8 + static_cast<uint64_t>(order) * bps + plan.bits;
        if (bits < best.bits)
        {
            best.kind = Subframe::Fixed;
            best.order = order;
            best.rice = plan;
            best.bits = bits;
        }
    }
    return best;
}

void writeSubframe(BitWriter& w, const Subframe& plan, const int64_t* x, int n, int bps,
                   std::vector<int64_t>& residual)
{
    switch (plan.kind)
    {
    case Subframe::Constant:
        w.bits(0, 8); // zero bit, type 000000, no wasted bits
        w.signedBits(x[0], bps);
        return;
    case Subframe::Verbatim:
        w.bits(0b00000010, 8);
        for (int i = 0; i < n; ++i)
            w.signedBits(x[i], bps);
        return;
    case Subframe::Fixed:
        break;
    }
    w.bits(static_cast<uint64_t>(0b00010000 | (plan.order << 1)), 8); // type 001xxx
    for (int i = 0; i < plan.order; ++i)
        w.signedBits(x[i], bps);
    fixedResidual(x, n, plan.order, residual.data());
    w.bits(0, 2); // Rice, 4-bit parameters
    w.bits(static_cast<uint64_t>(plan.rice.partitionOrder), 4);
    const int parts = 1 << plan.rice.partitionOrder, size = n >> plan.rice.partitionOrder;
    for (int k = 0; k < parts; ++k)
    {
        const int param = plan.rice.params[static_cast<size_t>(k)];
        w.bits(static_cast<uint64_t>(param), 4);
        for (int i = k == 0 ? plan.order : k * size; i < (k + 1) * size; ++i)
        {
            const uint64_t u = zigzag(residual[i]);
            w.unary(u >> param);
            if (param > 0)
                w.bits(u & ((uint64_t { 1 } << param) - 1), param);
        }
    }
}

int sampleRateCode(uint32_t rate)
{
    switch (rate)
    {
    case 88200: return 0b0001;
    case 176400: return 0b0010;
    case 192000: return 0b0011;
    case 8000: return 0b0100;
    case 16000: return 0b0101;
    case 22050: return 0b0110;
    case 24000: return 0b0111;
    case 32000: return 0b1000;
    case 44100: return 0b1001;
    case 48000: return 0b1010;
    case 96000: return 0b1011;
    default: return 0; // from STREAMINFO
    }
}

// FLAC's "UTF-8" coding of the frame number
void writeUtf8(BitWriter& w, uint32_t v)
{
    if (v < 0x80)
    {
        w.bits(v, 8);
        return;
    }
    int bytes = v < 0x800 ? 2 : v < 0x10000 ? 3 : v < 0x200000 ? 4 : v < 0x4000000 ? 5 : 6;
    w.bits(((0xff00u >> bytes) & 0xff) | (v >> (6 * (bytes - 1))), 8);
    for (int i = bytes - 2; i >= 0; --i)
        w.bits(0x80 | ((v >> (6 * i)) & 0x3f), 8);
}

} // namespace

std::filesystem::path looperExportFile(uint32_t instanceId)
{
    return cacheDir() / ("looper-" + std::to_string(instanceId) + ".wav");
}

int32_t toPcm24(float sample)
{
    const double v = std::clamp(static_cast<double>(sample), -1.0, 1.0) * 8388607.0;
    return static_cast<int32_t>(std::lround(v));
}

std::string encodeWav(const float* left, const float* right, size_t frames, uint32_t sampleRate, bool float32)
{
    const uint32_t bytesPerSample = float32 ? 4 : 3;
    const auto dataBytes = static_cast<uint32_t>(frames * 2 * bytesPerSample);
    std::string out;
    out.reserve(44 + dataBytes);
    out += "RIFF";
    put32(out, 36 + dataBytes);
    out += "WAVEfmt ";
    put32(out, 16);
    put16(out, float32 ? 3 : 1); // IEEE float or PCM
    put16(out, 2);
    put32(out, sampleRate);
    put32(out, sampleRate * 2 * bytesPerSample);
    put16(out, 2 * bytesPerSample);
    put16(out, bytesPerSample * 8);
    out += "data";
    put32(out, dataBytes);
    for (size_t i = 0; i < frames; ++i)
    {
        for (const float* channel : { left, right })
        {
            if (float32)
            {
                uint32_t bits;
                std::memcpy(&bits, &channel[i], 4);
                put32(out, bits);
            }
            else
            {
                const auto v = static_cast<uint32_t>(toPcm24(channel[i]));
                out += static_cast<char>(v & 0xff);
                out += static_cast<char>((v >> 8) & 0xff);
                out += static_cast<char>((v >> 16) & 0xff);
            }
        }
    }
    return out;
}

std::optional<StereoAudio> decodeWav(std::string_view b)
{
    if (b.size() < 12 || b.substr(0, 4) != "RIFF" || b.substr(8, 4) != "WAVE")
        return std::nullopt;
    uint32_t format = 0, channels = 0, rate = 0, bits = 0;
    size_t at = 12;
    while (at + 8 <= b.size())
    {
        const std::string_view id = b.substr(at, 4);
        const uint32_t size = get32(b, at + 4);
        const size_t body = at + 8;
        if (body + size > b.size())
            return std::nullopt;
        if (id == "fmt " && size >= 16)
        {
            format = get16(b, body);
            channels = get16(b, body + 2);
            rate = get32(b, body + 4);
            bits = get16(b, body + 14);
            if (format == 0xfffe && size >= 26)
                format = get16(b, body + 24); // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with the format
        }
        else if (id == "data")
        {
            const bool ok = (format == 3 && bits == 32) || (format == 1 && (bits == 16 || bits == 24));
            if (!ok || channels < 1 || channels > 2 || rate == 0)
                return std::nullopt;
            const size_t bytesPerSample = bits / 8;
            const size_t frames = size / (bytesPerSample * channels);
            StereoAudio audio;
            audio.sampleRate = rate;
            audio.left.resize(frames);
            audio.right.resize(frames);
            for (size_t i = 0; i < frames; ++i)
            {
                for (uint32_t ch = 0; ch < 2; ++ch)
                {
                    const size_t p = body + (i * channels + std::min(ch, channels - 1)) * bytesPerSample;
                    float v = 0.0f;
                    if (format == 3)
                    {
                        const uint32_t raw = get32(b, p);
                        std::memcpy(&v, &raw, 4);
                    }
                    else if (bits == 16)
                        v = static_cast<int16_t>(get16(b, p)) / 32768.0f;
                    else
                    {
                        int32_t s = static_cast<int32_t>(get16(b, p) | static_cast<uint32_t>(static_cast<uint8_t>(b[p + 2])) << 16);
                        if (s & 0x800000)
                            s -= 0x1000000;
                        v = static_cast<float>(s / 8388607.0);
                    }
                    (ch == 0 ? audio.left : audio.right)[i] = v;
                }
            }
            return audio;
        }
        at = body + size + (size & 1);
    }
    return std::nullopt;
}

std::string encodeFlac(const float* left, const float* right, size_t frames, uint32_t sampleRate)
{
    constexpr int bps = 24;
    std::string out = "fLaC";
    // STREAMINFO, the only (so last) metadata block
    {
        BitWriter w;
        w.bits(1, 1);
        w.bits(0, 7);
        w.bits(34, 24);
        w.bits(kBlockSize, 16); // min block size (the last block, shorter, doesn't count)
        w.bits(kBlockSize, 16);
        w.bits(0, 24); // frame sizes unknown
        w.bits(0, 24);
        w.bits(sampleRate, 20);
        w.bits(1, 3); // two channels
        w.bits(bps - 1, 5);
        w.bits(frames, 36);
        w.bits(0, 64); // MD5 unset
        w.bits(0, 64);
        out.append(reinterpret_cast<const char*>(w.bytes().data()), w.bytes().size());
    }

    std::vector<int64_t> l(kBlockSize), r(kBlockSize), mid(kBlockSize), side(kBlockSize), residual(kBlockSize);
    std::vector<uint64_t> u(kBlockSize);
    uint32_t frameNumber = 0;
    for (size_t start = 0; start < frames; start += kBlockSize, ++frameNumber)
    {
        const int n = static_cast<int>(std::min<size_t>(kBlockSize, frames - start));
        for (int i = 0; i < n; ++i)
        {
            l[i] = toPcm24(left[start + i]);
            r[i] = toPcm24(right[start + i]);
            side[i] = l[i] - r[i];
            mid[i] = (l[i] + r[i]) >> 1;
        }
        // the four stereo modes: independent, left/side, right/side, mid/side (side takes a bit more)
        const Subframe pl = planSubframe(l.data(), n, bps, residual, u);
        const Subframe pr = planSubframe(r.data(), n, bps, residual, u);
        const Subframe ps = planSubframe(side.data(), n, bps + 1, residual, u);
        const Subframe pm = planSubframe(mid.data(), n, bps, residual, u);
        const uint64_t costs[4] = { pl.bits + pr.bits, pl.bits + ps.bits, ps.bits + pr.bits, pm.bits + ps.bits };
        const int mode = static_cast<int>(std::min_element(costs, costs + 4) - costs);

        BitWriter w;
        w.bits(0xfff8, 16); // sync, fixed block size
        const int sizeCode = n == kBlockSize ? 12 : 7;
        w.bits(static_cast<uint64_t>(sizeCode), 4);
        w.bits(static_cast<uint64_t>(sampleRateCode(sampleRate)), 4);
        w.bits(mode == 0 ? 1 : static_cast<uint64_t>(7 + mode), 4); // 0001 independent, 1000 L/S, 1001 R/S, 1010 M/S
        w.bits(0b110, 3); // 24 bits
        w.bits(0, 1);
        writeUtf8(w, frameNumber);
        if (sizeCode == 7)
            w.bits(static_cast<uint64_t>(n - 1), 16);
        w.align();
        w.bits(crc8(w.bytes().data(), w.bytes().size()), 8);
        switch (mode)
        {
        case 0:
            writeSubframe(w, pl, l.data(), n, bps, residual);
            writeSubframe(w, pr, r.data(), n, bps, residual);
            break;
        case 1:
            writeSubframe(w, pl, l.data(), n, bps, residual);
            writeSubframe(w, ps, side.data(), n, bps + 1, residual);
            break;
        case 2:
            writeSubframe(w, ps, side.data(), n, bps + 1, residual);
            writeSubframe(w, pr, r.data(), n, bps, residual);
            break;
        default:
            writeSubframe(w, pm, mid.data(), n, bps, residual);
            writeSubframe(w, ps, side.data(), n, bps + 1, residual);
            break;
        }
        w.align();
        const uint16_t crc = crc16(w.bytes().data(), w.bytes().size());
        w.bits(crc, 16);
        out.append(reinterpret_cast<const char*>(w.bytes().data()), w.bytes().size());
    }
    return out;
}

} // namespace xythos
