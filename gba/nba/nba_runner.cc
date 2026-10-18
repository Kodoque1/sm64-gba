/*
 * Headless NanoBoyAdvance runner for the demo's bench ROM.
 *
 * Boots the ROM with the boot screen skipped and the stand-in BIOS
 * (gba/nba/mini_bios.s), runs it until the bench marks its save-RAM record
 * done (or the frame limit), and leaves the save RAM in a .sav file for
 * tools/bench_report.py.
 *
 *   nba_runner rom.gba bios.bin out.sav [max_gba_frames]
 */
#include <nba/core.hh>
#include <nba/rom/backup/sram.hh>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

extern "C" {
#include "bench.h"
}

static std::vector<u8> read_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "nba_runner: cannot read %s\n", path);
        std::exit(2);
    }
    return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s rom.gba bios.bin out.sav [max_gba_frames]\n", argv[0]);
        return 2;
    }
    long maxFrames = argc > 4 ? std::atol(argv[4]) : 20000;

    auto config = std::make_shared<nba::Config>();
    config->skip_bios = true;
    auto core = nba::CreateCore(config);
    core->Attach(read_file(argv[2]));

    std::filesystem::remove(argv[3]); /* start from erased save RAM (0xFF) */
    auto sram = std::make_unique<nba::SRAM>(argv[3]);
    nba::SRAM *save = sram.get();
    core->Attach(nba::ROM{read_file(argv[1]), std::move(sram), nullptr});
    core->Reset();

    long frames = 0;
    while (frames < maxFrames) {
        core->RunForOneFrame();
        frames++;
        if (frames % 60 == 0 && save->Read(BENCH_STATE_OFS) == BENCH_DONE) break;
    }
    if (save->Read(BENCH_STATE_OFS) != BENCH_DONE) {
        std::fprintf(stderr, "nba_runner: the bench did not finish within %ld frames\n", maxFrames);
        return 3;
    }
    return 0;
}
