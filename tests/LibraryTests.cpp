// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "engine/blocks/NamBlock.hpp"
#include "state/Library.hpp"
#include "state/LibraryScanner.hpp"
#include "state/Paths.hpp"

#include <miniz.h>

#include <filesystem>
#include <fstream>
#include <thread>

using namespace xythos;
namespace fs = std::filesystem;

namespace {

void writeText(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

void writeZip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files)
{
    fs::create_directories(path.parent_path());
    mz_zip_archive zip {};
    REQUIRE(mz_zip_writer_init_file(&zip, pathToUtf8(path).c_str(), 0));
    for (const auto& [name, data] : files)
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
}

const std::string kAmpA = R"({"version": "0.5.0", "metadata": {"gear_make": "Fender", "gear_model": "Deluxe",
                              "gear_type": "amp"}})";
const std::string kAmpB = R"({"version": "0.5.0", "metadata": {"name": "Pedal B", "gear_type": "pedal"}})";
const std::string kAmpC = R"({"version": "0.5.0"})";

// A library folder with a plain file, a file and an image (ignored) in a subfolder, and a zip with a file, an image
// and an IR
struct LibraryFixture {
    fs::path root = fs::temp_directory_path() / "xythos-test-library";

    LibraryFixture()
    {
        fs::remove_all(root);
        testing::setEnv("XYTHOS_CACHE_DIR", pathToUtf8(root / "cache"));
        writeText(root / "lib" / "A Amp.nam", kAmpA);
        writeText(root / "lib" / "sub" / "b.nam", kAmpB);
        writeText(root / "lib" / "sub" / "b.png", "png");
        writeText(root / "lib" / "notes.txt", "not a model");
        writeZip(root / "lib" / "Pack.zip", {
            { "Amps/c.nam", kAmpC },
            { "Amps/c.jpg", "jpg" },
            { "cab.wav", "RIFF" },
            { "__MACOSX/Amps/._c.nam", "junk" },
        });
    }
};

} // namespace

TEST_CASE("archive paths")
{
    CHECK_FALSE(splitArchivePath("/a/b.nam"));
    const auto split = splitArchivePath("/a/Pack.zip!/x/y.nam");
    REQUIRE(split);
    CHECK(split->archive == "/a/Pack.zip");
    CHECK(split->entry == "x/y.nam");
}

TEST_CASE("library scan finds files in folders and zips, with hashes and info")
{
    LibraryFixture fixture;
    const fs::path lib = fixture.root / "lib";
    LibraryIndex index;
    const std::vector<LibraryEntry> nam = scanLibrary(lib, LibraryKind::Nam, index);
    REQUIRE(nam.size() == 3);

    CHECK(nam[0].name == "A Amp");
    CHECK(nam[0].folder == "");
    CHECK(nam[0].hash == hashBytes(kAmpA));
    CHECK(nam[0].info == "Fender Deluxe");
    CHECK(nam[0].gearType == "amp");

    CHECK(nam[1].name == "c");
    CHECK(nam[1].folder == "Pack/Amps");
    CHECK(nam[1].path == pathToUtf8(lib / "Pack.zip") + "!/Amps/c.nam");
    CHECK(nam[1].hash == hashBytes(kAmpC));

    CHECK(nam[2].name == "b");
    CHECK(nam[2].folder == "sub");
    CHECK(nam[2].info == "Pedal B");
    CHECK(nam[2].gearType == "pedal");

    LibraryIndex irIndex;
    const std::vector<LibraryEntry> ir = scanLibrary(lib, LibraryKind::Ir, irIndex);
    REQUIRE(ir.size() == 1);
    CHECK(ir[0].name == "cab");
    CHECK(ir[0].folder == "Pack");
}

TEST_CASE("NAM gear types map to the categories, from NAM's trainer and TONE3000")
{
    CHECK(namGear("amp_cab") == NamGear::AmpCab);
    CHECK(namGear("amp-cab") == NamGear::AmpCab);
    CHECK(namGear("amp_pedal_cab") == NamGear::AmpCab);
    CHECK(namGear("full-rig") == NamGear::AmpCab);
    CHECK(namGear("amp") == NamGear::Amp);
    CHECK(namGear("pedal_amp") == NamGear::Amp);
    CHECK(namGear("preamp") == NamGear::Amp);
    CHECK(namGear("cab") == NamGear::Cab);
    CHECK(namGear("ir") == NamGear::Cab);
    CHECK(namGear("Pedal") == NamGear::Pedal);
    CHECK(namGear("outboard") == NamGear::Outboard);
    CHECK(namGear("studio") == NamGear::Outboard);
    CHECK(namGear("space") == NamGear::Space);
    CHECK(namGear("experimental") == NamGear::Experimental);
    CHECK(namGear("pedal_amp_cab") == NamGear::AmpCab);
    CHECK(namGear("") == NamGear::Unknown);
    CHECK(namGear("toaster") == NamGear::Unknown);
    CHECK(std::string(namGearName(NamGear::AmpCab)) == "Amp + Cab");
    CHECK(std::string(namGearName(NamGear::Amp)) == "Amp Head");
    CHECK(std::string(namGearName(NamGear::Cab)) == "Cabinet");
    CHECK(std::string(namGearName(NamGear::Space)) == "Spaces");
    CHECK(std::string(namGearName(NamGear::Unknown)).empty());
}

TEST_CASE("NAM file paths say whether a model is a full rig or the amp alone")
{
    // full rigs, by the file's name
    CHECK(rigInName("/nam/Marshall JCM800 Full Rig.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/jcm800_fullrig_g7.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/Fender Deluxe Combo.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/vox_combos_ac15.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/Plexi Half-Stack.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/5150 stack crunch.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/Rectifier amp+cab.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/Rectifier Amp + Cab.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/Rectifier with cab.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/Mesa 4x12 V30.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/AC30_2x12.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/JCM800 SM57.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/JCM800 Mic'd.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/Marshall head into 4x12.nam") == RigInName::FullRig); // the cab wins over "head"
    // the amp alone
    CHECK(rigInName("/nam/Marshall JCM800 head.nam") == RigInName::Head);
    CHECK(rigInName("/nam/JCM800_DI.nam") == RigInName::Head);
    CHECK(rigInName("/nam/JCM800 amp only.nam") == RigInName::Head);
    CHECK(rigInName("/nam/JCM800 no cab.nam") == RigInName::Head);
    CHECK(rigInName("/nam/Combo amp, no cab.nam") == RigInName::Head); // said outright, it wins
    CHECK(rigInName("/nam/SLO Preamp.nam") == RigInName::Head);
    // nothing said
    CHECK(rigInName("/nam/Marshall JCM800.nam") == RigInName::Unknown);
    CHECK(rigInName("/nam/Kombo Headroom Stacker2x.nam") == RigInName::Unknown); // not words
    CHECK(rigInName("/nam/Model 1x120.nam") == RigInName::Unknown);
    CHECK(rigInName("") == RigInName::Unknown);
    // the folders, closest first; the file's name wins over them
    CHECK(rigInName("/home/me/NAM/Full Rigs/Marshall/JCM800.nam") == RigInName::FullRig);
    CHECK(rigInName("/home/me/NAM/Full Rigs/Heads/JCM800.nam") == RigInName::Head);
    CHECK(rigInName("/home/me/NAM/Heads/JCM800 4x12.nam") == RigInName::FullRig);
    CHECK(rigInName("C:\\NAM\\Combos\\Deluxe.nam") == RigInName::FullRig);
    CHECK(rigInName("/nam/Stacks.zip!/Marshall/JCM800.nam") == RigInName::FullRig);

    // the gear type: names count where the metadata says "amp" or nothing
    CHECK(namGear("amp", "/nam/JCM800 full rig.nam") == NamGear::AmpCab);
    CHECK(namGear("", "/nam/Combos/Deluxe.nam") == NamGear::AmpCab);
    CHECK(namGear("", "/nam/JCM800 head.nam") == NamGear::Amp);
    CHECK(namGear("", "/nam/JCM800.nam") == NamGear::Unknown);
    CHECK(namGear("amp", "/nam/JCM800.nam") == NamGear::Amp);
    CHECK(namGear("amp_cab", "/nam/JCM800 head.nam") == NamGear::AmpCab); // the metadata said so outright
    CHECK(namGear("pedal", "/nam/Combo Drive.nam") == NamGear::Pedal);
    CHECK(namGear("cab", "/nam/4x12 SM57.nam") == NamGear::Cab);
    CHECK(namGear("outboard", "/nam/Combo.nam") == NamGear::Outboard);
}

TEST_CASE("NAM files describe their architecture")
{
    auto wavenet = [](int a, int b) {
        return R"({"architecture": "WaveNet", "config": {"layers": [{"channels": )" + std::to_string(a) +
               R"(}, {"channels": )" + std::to_string(b) + "}]}}";
    };
    CHECK(describeNamFile(wavenet(16, 8)).arch == "A1 Standard");
    CHECK(describeNamFile(wavenet(12, 6)).arch == "A1 Lite");
    CHECK(describeNamFile(wavenet(8, 4)).arch == "A1 Feather");
    CHECK(describeNamFile(wavenet(4, 2)).arch == "A1 Nano");
    CHECK(describeNamFile(wavenet(20, 10)).arch == "A1");
    CHECK(describeNamFile(R"({"architecture": "SlimmableContainer", "config": {}})").arch == "A2");
    CHECK(describeNamFile(R"({"architecture": "WaveNet", "config": {"layers": [{"channels": 3, "slimmable": {}}]}})")
              .arch == "A2");
    CHECK(describeNamFile(R"({"architecture": "WaveNet", "config": {"layers": [{"channels": 8, "kernel_sizes": [3]}]}})")
              .arch == "A2");
    CHECK(describeNamFile(R"({"architecture": "LSTM", "config": {}})").arch == "LSTM");
    CHECK(describeNamFile("not json").arch.empty());

    const NamFileInfo info = describeNamFile(
        R"({"architecture": "LSTM", "metadata": {"name": "Plexi", "gear_type": "amp-cab"}})");
    CHECK(info.info == "Plexi");
    CHECK(info.gearType == "amp-cab");
}

#ifdef XYTHOS_EXAMPLE_MODELS
TEST_CASE("NAM Core's example models' architectures")
{
    const fs::path dir = XYTHOS_EXAMPLE_MODELS;
    auto arch = [&](const char* name) {
        const std::optional<std::string> bytes = readLibraryFile(pathToUtf8(dir / name));
        REQUIRE(bytes);
        return describeNamFile(*bytes).arch;
    };
    CHECK(arch("wavenet_a1_standard.nam") == "A1 Standard");
    CHECK(arch("A2.nam") == "A2");
    CHECK(arch("slimmable_wavenet.nam") == "A2");
    CHECK(arch("lstm.nam") == "LSTM");
}
#endif

TEST_CASE("library rescans reuse the index for unchanged files and rehash changed ones")
{
    LibraryFixture fixture;
    const fs::path lib = fixture.root / "lib";
    LibraryIndex index;
    std::vector<LibraryEntry> first = scanLibrary(lib, LibraryKind::Nam, index);
    REQUIRE(index.size() == 3);

    // the index round-trips through its file
    const fs::path indexPath = fixture.root / "cache" / "index.json";
    REQUIRE(index.save(indexPath));
    LibraryIndex loaded;
    loaded.load(indexPath);
    CHECK(loaded.size() == 3);

    // An index entry that still matches size and time is trusted (proves files aren't reread)
    LibraryEntry fake = first[0];
    fake.hash = "cached";
    loaded.put(fake);
    CHECK(scanLibrary(lib, LibraryKind::Nam, loaded)[0].hash == "cached");

    // A changed file is reread
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writeText(lib / "A Amp.nam", kAmpA + " ");
    fs::last_write_time(lib / "A Amp.nam", fs::last_write_time(lib / "A Amp.nam") + std::chrono::seconds(2));
    CHECK(scanLibrary(lib, LibraryKind::Nam, loaded)[0].hash == hashBytes(kAmpA + " "));

    // A removed file leaves the index
    fs::remove(lib / "sub" / "b.nam");
    CHECK(scanLibrary(lib, LibraryKind::Nam, loaded).size() == 2);
    CHECK(loaded.size() == 2);
}

TEST_CASE("a cancelled scan leaves the index alone")
{
    LibraryFixture fixture;
    LibraryIndex index;
    LibraryEntry kept;
    kept.path = "/kept.nam";
    index.put(kept);
    const std::atomic<bool> cancel { true };
    scanLibrary(fixture.root / "lib", LibraryKind::Nam, index, &cancel);
    CHECK(index.size() == 1);
}

TEST_CASE("library files inside zips can be read, hashed and extracted")
{
    LibraryFixture fixture;
    const std::string zipped = pathToUtf8(fixture.root / "lib" / "Pack.zip") + "!/Amps/c.nam";
    CHECK(readLibraryFile(zipped) == kAmpC);
    CHECK(hashLibraryFile(zipped) == hashBytes(kAmpC));
    CHECK_FALSE(readLibraryFile(pathToUtf8(fixture.root / "lib" / "Pack.zip") + "!/missing.nam"));

    const std::optional<std::string> extracted = materializeLibraryFile(zipped);
    REQUIRE(extracted);
    CHECK(fs::path(pathFromUtf8(*extracted)).filename() == "c.nam");
    CHECK(readFile(pathFromUtf8(*extracted)) == kAmpC);
    CHECK(materializeLibraryFile(zipped) == extracted); // reused

    const std::string plain = pathToUtf8(fixture.root / "lib" / "A Amp.nam");
    CHECK(materializeLibraryFile(plain) == plain);
}

TEST_CASE("a NAM block loads a model from inside a zip")
{
    LibraryFixture fixture;
    const fs::path model = fs::path(XYTHOS_EXAMPLE_MODELS) / "lstm.nam";
    const std::optional<std::string> bytes = readFile(model);
    REQUIRE(bytes);
    writeZip(fixture.root / "models.zip", { { "folder/lstm.nam", *bytes } });

    std::unique_ptr<Block> block = kNamBlockDescriptor.create();
    block->prepare(48000.0, 128);
    CHECK(block->configure({ { "path", pathToUtf8(fixture.root / "models.zip") + "!/folder/lstm.nam" } }));
}

TEST_CASE("the library scanner scans both folders in the background")
{
    LibraryFixture fixture;
    LibraryScanner scanner;
    scanner.start(pathToUtf8(fixture.root / "lib"), "");
    scanner.wait();
    CHECK_FALSE(scanner.busy());
    std::optional<LibraryScanner::Result> result = scanner.poll();
    REQUIRE(result);
    CHECK(result->nam.size() == 3);
    CHECK(result->ir.empty());
    CHECK_FALSE(scanner.poll()); // delivered once
    // the index was saved for the next scan
    LibraryIndex index;
    index.load(LibraryIndex::defaultPath(LibraryKind::Nam));
    CHECK(index.size() == 3);
}
