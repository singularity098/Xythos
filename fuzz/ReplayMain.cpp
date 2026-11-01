// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// xythos-fuzz-replay: runs every fuzz target without libFuzzer (any compiler, any platform).
//
//   xythos-fuzz-replay <corpus-dir> [--mutations N] [--seed S] [--only name]
//       For each target: every file in <corpus-dir>/<name>/, then N deterministic mutations of them (default 200).
//       Each input is written to crash-<name>.bin in the working directory before it runs, and removed after the
//       run passes, so a failure leaves the input behind for --target.
//   xythos-fuzz-replay --target <name> <file>...
//       Runs one target on the given files (to reproduce a crash a libFuzzer run saved).
//   xythos-fuzz-replay --list

#include "FuzzTarget.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace xythos::fuzz {

namespace {

std::map<std::string, TargetFn>& targets()
{
    static std::map<std::string, TargetFn> map;
    return map;
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

int run(TargetFn fn, const std::string& bytes)
{
    // a copy in its own allocation, so ASan builds catch reads past the end
    std::vector<uint8_t> copy(bytes.begin(), bytes.end());
    return fn(copy.empty() ? nullptr : copy.data(), copy.size());
}

// One libFuzzer-style mutation: flip, overwrite with an interesting value, insert, erase, duplicate or splice
std::string mutate(const std::vector<std::string>& corpus, std::mt19937& rng)
{
    std::string s = corpus.empty() ? std::string() : corpus[rng() % corpus.size()];
    const int edits = 1 + static_cast<int>(rng() % 8);
    for (int e = 0; e < edits; ++e)
    {
        const size_t at = s.empty() ? 0 : rng() % s.size();
        switch (rng() % 7)
        {
        case 0:
            if (!s.empty())
                s[at] = static_cast<char>(s[at] ^ (1 << (rng() % 8)));
            break;
        case 1:
            if (!s.empty())
            {
                static constexpr unsigned char kInteresting[] = { 0, 1, 0x7f, 0x80, 0xff, '"', '{', '[', ']', '}',
                                                                  '-', '9', '.', 'e', '\\', '/' };
                s[at] = static_cast<char>(kInteresting[rng() % sizeof kInteresting]);
            }
            break;
        case 2:
            s.insert(at, 1 + rng() % 4, static_cast<char>(rng()));
            break;
        case 3:
            if (!s.empty())
                s.erase(at, 1 + rng() % std::min<size_t>(16, s.size() - at));
            break;
        case 4:
            if (!s.empty() && s.size() < 1 << 16)
            {
                const size_t length = 1 + rng() % std::min<size_t>(64, s.size() - at);
                s.insert(rng() % (s.size() + 1), s.substr(at, length));
            }
            break;
        case 5:
            if (!corpus.empty())
            {
                const std::string& other = corpus[rng() % corpus.size()];
                if (!other.empty())
                    s = s.substr(0, at) + other.substr(rng() % other.size());
            }
            break;
        case 6:
            s.resize(at); // truncate
            break;
        }
    }
    return s;
}

int usage()
{
    std::cerr << "usage: xythos-fuzz-replay <corpus-dir> [--mutations N] [--seed S] [--only name]\n"
                 "       xythos-fuzz-replay --target <name> <file>...\n"
                 "       xythos-fuzz-replay --list\n";
    return 2;
}

} // namespace

bool registerTarget(const char* name, TargetFn fn)
{
    targets()[name] = fn;
    return true;
}

} // namespace xythos::fuzz

int main(int argc, char** argv)
{
    using namespace xythos::fuzz;
    namespace fs = std::filesystem;

    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty())
        return usage();

    if (args[0] == "--list")
    {
        for (const auto& [name, fn] : targets())
            std::cout << name << "\n";
        return 0;
    }

    if (args[0] == "--target")
    {
        if (args.size() < 3 || !targets().count(args[1]))
            return usage();
        for (size_t i = 2; i < args.size(); ++i)
        {
            std::cout << args[1] << ": " << args[i] << std::endl;
            run(targets()[args[1]], readFile(args[i]));
        }
        std::cout << "ok" << std::endl;
        return 0;
    }

    const fs::path root = args[0];
    int mutations = 200;
    uint32_t seed = 1;
    std::string only;
    for (size_t i = 1; i + 1 < args.size(); i += 2)
    {
        if (args[i] == "--mutations")
            mutations = std::stoi(args[i + 1]);
        else if (args[i] == "--seed")
            seed = static_cast<uint32_t>(std::stoul(args[i + 1]));
        else if (args[i] == "--only")
            only = args[i + 1];
        else
            return usage();
    }

    for (const auto& [name, fn] : targets())
    {
        if (!only.empty() && name != only)
            continue;
        const auto start = std::chrono::steady_clock::now();
        std::vector<std::string> corpus;
        std::vector<fs::path> files;
        std::error_code ec;
        if (fs::is_directory(root / name, ec))
        {
            for (const auto& entry : fs::directory_iterator(root / name))
            {
                if (entry.is_regular_file())
                    files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files)
        {
            corpus.push_back(readFile(file));
            run(fn, corpus.back());
        }
        run(fn, {});

        // the same mutations on every platform and run: a failure here reproduces with the same seed
        std::mt19937 rng(seed ^ static_cast<uint32_t>(std::hash<std::string> {}(name) & 0xffffffffu));
        const fs::path crashFile = "crash-" + name + ".bin";
        for (int i = 0; i < mutations; ++i)
        {
            const std::string input = mutate(corpus, rng);
            {
                std::ofstream out(crashFile, std::ios::binary | std::ios::trunc);
                out.write(input.data(), static_cast<std::streamsize>(input.size()));
            }
            run(fn, input);
        }
        fs::remove(crashFile, ec);

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        std::cout << name << ": " << files.size() << " corpus files, " << mutations << " mutations, " << ms.count()
                  << " ms" << std::endl;
    }
    return 0;
}
