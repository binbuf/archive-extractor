#include "archive_core/detect.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using ae::Backend;
using ae::DetectFile;
using ae::Filter;
using ae::Format;
using ae::Kind;
using ae::PipelinePlan;
using ae::PlanFromHeader;
using ae::ReroutedForEncryption;
using ae::SniffMagic;
using ae::VolumeFamily;

namespace {

#ifndef AE_TEST_ASSETS_DIR
#error "AE_TEST_ASSETS_DIR must be defined (see CMakeLists.txt)."
#endif

fs::path AssetPath(const char* name) {
    return fs::path(AE_TEST_ASSETS_DIR) / name;
}

// Read the leading bytes (header window) of a corpus asset.
std::string ReadHeader(const char* name, std::size_t n = ae::kHeaderWindow) {
    std::ifstream in(AssetPath(name), std::ios::binary);
    if (!in) return {};
    std::string buf(n, '\0');
    in.read(buf.data(), static_cast<std::streamsize>(n));
    buf.resize(static_cast<std::size_t>(in.gcount()));
    return buf;
}

bool HasFilter(const PipelinePlan& p, Filter f) {
    return std::find(p.filters.begin(), p.filters.end(), f) != p.filters.end();
}

}  // namespace

// --- Magic-byte sniffing over REAL corpus headers --------------------------

TEST(SniffMagic, RecognizesRealHeaders) {
    EXPECT_EQ(SniffMagic(ReadHeader("zip__single-file.zip")), Format::Zip);
    EXPECT_EQ(SniffMagic(ReadHeader("7z__single-file.7z")), Format::SevenZip);
    EXPECT_EQ(SniffMagic(ReadHeader("tar__single-file.tar")), Format::Tar);
    EXPECT_EQ(SniffMagic(ReadHeader("document.txt.gz")), Format::Gzip);
    EXPECT_EQ(SniffMagic(ReadHeader("notes.txt.bz2")), Format::Bzip2);
    EXPECT_EQ(SniffMagic(ReadHeader("data.csv.xz")), Format::Xz);
    EXPECT_EQ(SniffMagic(ReadHeader("payload.json.zst")), Format::Zstd);
    EXPECT_EQ(SniffMagic(ReadHeader("frame.bin.lz4")), Format::Lz4);
}

TEST(SniffMagic, CompoundTarHeadersSniffAsTheirFilterStream) {
    // A .tar.gz's first bytes are gzip's; the tar magic is inside the stream.
    EXPECT_EQ(SniffMagic(ReadHeader("targz__single-file.tar.gz")), Format::Gzip);
    EXPECT_EQ(SniffMagic(ReadHeader("tarxz__single-file.tar.xz")), Format::Xz);
    EXPECT_EQ(SniffMagic(ReadHeader("tarbz2__single-file.tar.bz2")),
              Format::Bzip2);
    EXPECT_EQ(SniffMagic(ReadHeader("tarzst__single-file.tar.zst")),
              Format::Zstd);
    EXPECT_EQ(SniffMagic(ReadHeader("tarlz4__single-file.tar.lz4")),
              Format::Lz4);
}

TEST(SniffMagic, BrotliHasNoMagic) {
    // brotli streams carry no recognizable signature.
    EXPECT_FALSE(SniffMagic(ReadHeader("page.html.br")).has_value());
    EXPECT_FALSE(SniffMagic(ReadHeader("tarbr__single-file.tar.br")).has_value());
}

TEST(SniffMagic, UnknownContentReturnsNullopt) {
    EXPECT_FALSE(SniffMagic(ReadHeader("edge-unknown.bin")).has_value());
}

TEST(SniffMagic, ShortBufferDoesNotReachTarProbe) {
    // tar magic is at offset 257; a tiny buffer must not crash or false-match.
    EXPECT_FALSE(SniffMagic(std::string_view("ust", 3)).has_value());
    EXPECT_FALSE(SniffMagic(nullptr, 0).has_value());
}

// --- Plain containers ------------------------------------------------------

TEST(Plan, Zip) {
    const auto h = ReadHeader("zip__multi-files.zip");
    const auto p = PlanFromHeader(L"C:\\dl\\zip__multi-files.zip", h);
    EXPECT_EQ(p.format, Format::Zip);
    EXPECT_EQ(p.backend, Backend::LibArchive);
    EXPECT_EQ(p.kind, Kind::Container);
    EXPECT_TRUE(p.filters.empty());
    EXPECT_EQ(p.stem, L"zip__multi-files");
    EXPECT_TRUE(p.outputNameHint.empty());
    EXPECT_FALSE(p.extensionOverriddenByContent);
    EXPECT_EQ(p.volumeFamily, VolumeFamily::NotMultiVolume);
}

TEST(Plan, SevenZipPlainRoutesLibArchive) {
    const auto h = ReadHeader("7z__multi-files.7z");
    const auto p = PlanFromHeader(L"data.7z", h);
    EXPECT_EQ(p.format, Format::SevenZip);
    EXPECT_EQ(p.backend, Backend::LibArchive);  // plain 7z -> libarchive
    EXPECT_EQ(p.kind, Kind::Container);
    EXPECT_EQ(p.stem, L"data");
}

TEST(Plan, TarPlain) {
    const auto h = ReadHeader("tar__single-folder.tar");
    const auto p = PlanFromHeader(L"x/tar__single-folder.tar", h);
    EXPECT_EQ(p.format, Format::Tar);
    EXPECT_EQ(p.backend, Backend::LibArchive);
    EXPECT_EQ(p.kind, Kind::Container);
    EXPECT_TRUE(p.filters.empty());
    EXPECT_EQ(p.stem, L"tar__single-folder");
}

TEST(Plan, RarRoutesSevenZipDll) {
    // No corpus .rar asset (proprietary) -> drive by extension only.
    const auto p = PlanFromHeader(L"movie.rar", std::string_view{});
    EXPECT_EQ(p.format, Format::Rar);
    EXPECT_EQ(p.backend, Backend::SevenZipDll);
    EXPECT_EQ(p.kind, Kind::Container);
    EXPECT_EQ(p.stem, L"movie");
}

// --- Bare single-stream ----------------------------------------------------

TEST(Plan, BareGzipSingleStream) {
    const auto h = ReadHeader("document.txt.gz");
    const auto p = PlanFromHeader(L"document.txt.gz", h);
    EXPECT_EQ(p.format, Format::Gzip);
    EXPECT_EQ(p.kind, Kind::SingleStream);
    EXPECT_EQ(p.backend, Backend::LibArchive);
    ASSERT_EQ(p.filters.size(), 1u);
    EXPECT_EQ(p.filters[0], Filter::Gzip);
    EXPECT_EQ(p.stem, L"document.txt");        // .gz stripped
    EXPECT_EQ(p.outputNameHint, L"document.txt");
}

TEST(Plan, BareSingleStreamsAllFormats) {
    struct C { const char* asset; const wchar_t* name; Format f; Filter fil;
               const wchar_t* inner; };
    const C cases[] = {
        {"notes.txt.bz2", L"notes.txt.bz2", Format::Bzip2, Filter::Bzip2,
         L"notes.txt"},
        {"data.csv.xz", L"data.csv.xz", Format::Xz, Filter::Xz, L"data.csv"},
        {"payload.json.zst", L"payload.json.zst", Format::Zstd, Filter::Zstd,
         L"payload.json"},
        {"frame.bin.lz4", L"frame.bin.lz4", Format::Lz4, Filter::Lz4,
         L"frame.bin"},
    };
    for (const auto& c : cases) {
        const auto h = ReadHeader(c.asset);
        const auto p = PlanFromHeader(c.name, h);
        EXPECT_EQ(p.format, c.f) << c.asset;
        EXPECT_EQ(p.kind, Kind::SingleStream) << c.asset;
        EXPECT_TRUE(HasFilter(p, c.fil)) << c.asset;
        EXPECT_EQ(p.outputNameHint, c.inner) << c.asset;
    }
}

// --- Brotli (extension only) -----------------------------------------------

TEST(Plan, BrotliDetectedByExtensionOnly) {
    const auto h = ReadHeader("page.html.br");  // no magic in these bytes
    const auto p = PlanFromHeader(L"page.html.br", h);
    EXPECT_EQ(p.format, Format::Brotli);
    EXPECT_EQ(p.backend, Backend::Brotli);
    EXPECT_EQ(p.kind, Kind::SingleStream);
    ASSERT_EQ(p.filters.size(), 1u);
    EXPECT_EQ(p.filters[0], Filter::Brotli);
    EXPECT_EQ(p.outputNameHint, L"page.html");
    EXPECT_FALSE(p.extensionOverriddenByContent);
}

TEST(Plan, BrotliNoExtensionMeansUnsupported) {
    // Same brotli bytes but no .br extension -> cannot be detected.
    const auto h = ReadHeader("page.html.br");
    const auto p = PlanFromHeader(L"mystery", h);
    EXPECT_EQ(p.format, Format::Unknown);
    EXPECT_FALSE(p.supported());
}

TEST(Plan, TarBrPlansBrotliThenTar) {
    const auto h = ReadHeader("tarbr__single-file.tar.br");
    const auto p = PlanFromHeader(L"site/tarbr__single-file.tar.br", h);
    EXPECT_EQ(p.format, Format::Tar);
    EXPECT_EQ(p.kind, Kind::Container);   // a tar inside -> container
    // The brotli wrapping filter forces the brotli backend (libarchive cannot
    // decompress brotli); BrotliExtractor decodes to a temp tar then untars.
    EXPECT_EQ(p.backend, Backend::Brotli);
    ASSERT_EQ(p.filters.size(), 1u);
    EXPECT_EQ(p.filters[0], Filter::Brotli);   // brotli -> tar
    EXPECT_EQ(p.stem, L"tarbr__single-file");
    EXPECT_TRUE(p.outputNameHint.empty());     // container, not single-stream
}

// --- Compound (filter -> tar) forms ----------------------------------------

TEST(Plan, CompoundTarForms) {
    struct C { const char* asset; const wchar_t* name; Filter fil;
               const wchar_t* stem; };
    const C cases[] = {
        {"targz__multi-files.tar.gz", L"targz__multi-files.tar.gz",
         Filter::Gzip, L"targz__multi-files"},
        {"tarbz2__multi-files.tar.bz2", L"tarbz2__multi-files.tar.bz2",
         Filter::Bzip2, L"tarbz2__multi-files"},
        {"tarxz__multi-files.tar.xz", L"tarxz__multi-files.tar.xz",
         Filter::Xz, L"tarxz__multi-files"},
        {"tarzst__multi-files.tar.zst", L"tarzst__multi-files.tar.zst",
         Filter::Zstd, L"tarzst__multi-files"},
        {"tarlz4__multi-files.tar.lz4", L"tarlz4__multi-files.tar.lz4",
         Filter::Lz4, L"tarlz4__multi-files"},
        {"tgz__single-file.tgz", L"tgz__single-file.tgz", Filter::Gzip,
         L"tgz__single-file"},
        {"tbz2__single-file.tbz2", L"tbz2__single-file.tbz2", Filter::Bzip2,
         L"tbz2__single-file"},
        {"txz__single-file.txz", L"txz__single-file.txz", Filter::Xz,
         L"txz__single-file"},
    };
    for (const auto& c : cases) {
        const auto h = ReadHeader(c.asset);
        const auto p = PlanFromHeader(c.name, h);
        EXPECT_EQ(p.format, Format::Tar) << c.asset;
        EXPECT_EQ(p.kind, Kind::Container) << c.asset;
        EXPECT_EQ(p.backend, Backend::LibArchive) << c.asset;
        ASSERT_EQ(p.filters.size(), 1u) << c.asset;
        EXPECT_EQ(p.filters[0], c.fil) << c.asset;
        EXPECT_EQ(p.stem, c.stem) << c.asset;
        EXPECT_TRUE(p.outputNameHint.empty()) << c.asset;
        EXPECT_FALSE(p.extensionOverriddenByContent) << c.asset;
    }
}

TEST(Plan, CompoundShorthandTgzMatchesEvenWithoutContent) {
    const auto p = PlanFromHeader(L"backup.tgz", std::string_view{});
    EXPECT_EQ(p.format, Format::Tar);
    EXPECT_TRUE(HasFilter(p, Filter::Gzip));
    EXPECT_EQ(p.stem, L"backup");
}

// --- Content-vs-extension override -----------------------------------------

TEST(Plan, RenamedSevenZipAsZipDetectedByContent) {
    const auto h = ReadHeader("edge-renamed-7z-as-zip.zip");
    const auto p = PlanFromHeader(L"D:\\x\\edge-renamed-7z-as-zip.zip", h);
    EXPECT_EQ(p.format, Format::SevenZip);   // content wins
    EXPECT_EQ(p.kind, Kind::Container);
    EXPECT_EQ(p.backend, Backend::LibArchive);  // plain 7z -> libarchive
    EXPECT_TRUE(p.extensionOverriddenByContent);
    // Stem strips the (wrong) .zip extension from the name.
    EXPECT_EQ(p.stem, L"edge-renamed-7z-as-zip");
}

TEST(Plan, MatchingExtensionAndContentNotFlaggedOverridden) {
    const auto h = ReadHeader("zip__single-file.zip");
    const auto p = PlanFromHeader(L"zip__single-file.zip", h);
    EXPECT_EQ(p.format, Format::Zip);
    EXPECT_FALSE(p.extensionOverriddenByContent);
}

TEST(Plan, UnknownExtensionKnownContentDrivesDetection) {
    const auto h = ReadHeader("7z__single-file.7z");
    const auto p = PlanFromHeader(L"archive.dat", h);
    EXPECT_EQ(p.format, Format::SevenZip);
    EXPECT_EQ(p.kind, Kind::Container);
    // Unknown extension -> stem is the whole basename.
    EXPECT_EQ(p.stem, L"archive.dat");
    EXPECT_FALSE(p.extensionOverriddenByContent);  // no extension to override
}

// --- Multi-volume ----------------------------------------------------------

TEST(Plan, SevenZipSplitFirstPart) {
    const auto h = ReadHeader("edge-multivolume.7z.001");
    const auto p =
        PlanFromHeader(L"C:\\dl\\edge-multivolume.7z.001", h);
    EXPECT_EQ(p.format, Format::SevenZip);
    EXPECT_EQ(p.kind, Kind::Container);
    EXPECT_EQ(p.volumeFamily, VolumeFamily::SevenZip);
    EXPECT_EQ(p.stem, L"edge-multivolume");
    EXPECT_EQ(p.firstPartPath, L"C:\\dl\\edge-multivolume.7z.001");
}

TEST(Plan, SevenZipSplitNonFirstPartResolvesToFirst) {
    // A .002 has no magic of its own; extension family still resolves.
    const auto h = ReadHeader("edge-multivolume.7z.002");
    const auto p =
        PlanFromHeader(L"C:\\dl\\edge-multivolume.7z.002", h);
    EXPECT_EQ(p.volumeFamily, VolumeFamily::SevenZip);
    EXPECT_EQ(p.stem, L"edge-multivolume");
    EXPECT_EQ(p.firstPartPath, L"C:\\dl\\edge-multivolume.7z.001");
    // Routed as 7z even though this part's bytes are opaque.
    EXPECT_EQ(p.format, Format::SevenZip);
}

TEST(Plan, RarNewStylePartFirstPart) {
    const auto p = PlanFromHeader(L"C:\\dl\\movie.part1.rar", std::string_view{});
    EXPECT_EQ(p.format, Format::Rar);
    EXPECT_EQ(p.backend, Backend::SevenZipDll);
    EXPECT_EQ(p.volumeFamily, VolumeFamily::RarPart);
    EXPECT_EQ(p.stem, L"movie");
    EXPECT_EQ(p.firstPartPath, L"C:\\dl\\movie.part1.rar");
}

TEST(Plan, RarNewStylePartNonFirstResolvesToPart1) {
    const auto p = PlanFromHeader(L"C:\\dl\\movie.part3.rar", std::string_view{});
    EXPECT_EQ(p.volumeFamily, VolumeFamily::RarPart);
    EXPECT_EQ(p.stem, L"movie");
    EXPECT_EQ(p.firstPartPath, L"C:\\dl\\movie.part1.rar");
}

TEST(Plan, RarOldStyleContinuationResolvesToRar) {
    const auto p = PlanFromHeader(L"C:\\dl\\disc.r00", std::string_view{});
    EXPECT_EQ(p.format, Format::Rar);
    EXPECT_EQ(p.backend, Backend::SevenZipDll);
    EXPECT_EQ(p.volumeFamily, VolumeFamily::RarOld);
    EXPECT_EQ(p.stem, L"disc");
    EXPECT_EQ(p.firstPartPath, L"C:\\dl\\disc.rar");
}

TEST(Plan, ZipSpannedContinuationResolvesToZip) {
    const auto p = PlanFromHeader(L"C:\\dl\\set.z01", std::string_view{});
    EXPECT_EQ(p.format, Format::Zip);
    EXPECT_EQ(p.backend, Backend::LibArchive);
    EXPECT_EQ(p.volumeFamily, VolumeFamily::ZipSpanned);
    EXPECT_EQ(p.stem, L"set");
    EXPECT_EQ(p.firstPartPath, L"C:\\dl\\set.zip");
}

// --- Unsupported -----------------------------------------------------------

TEST(Plan, UnknownContentAndExtensionUnsupported) {
    const auto h = ReadHeader("edge-unknown.bin");
    const auto p = PlanFromHeader(L"edge-unknown.bin", h);
    EXPECT_EQ(p.format, Format::Unknown);
    EXPECT_EQ(p.backend, Backend::None);
    EXPECT_EQ(p.kind, Kind::Unknown);
    EXPECT_FALSE(p.supported());
}

TEST(Plan, EmptyEverythingUnsupported) {
    const auto p = PlanFromHeader(L"plainfile.txt", std::string_view{});
    EXPECT_FALSE(p.supported());
}

// --- Encryption re-route hook ----------------------------------------------

TEST(Reroute, EncryptedSevenZipGoesToDll) {
    auto p = PlanFromHeader(L"secret.7z", ReadHeader("7z-aes__password.7z"));
    EXPECT_EQ(p.backend, Backend::LibArchive);  // assumed clear up front
    EXPECT_EQ(ReroutedForEncryption(p, /*encrypted=*/true), Backend::SevenZipDll);
    EXPECT_EQ(ReroutedForEncryption(p, /*encrypted=*/false), Backend::LibArchive);
}

TEST(Reroute, EncryptedZipStaysLibArchive) {
    auto p = PlanFromHeader(L"secret.zip", ReadHeader("zip-aes__password.zip"));
    EXPECT_EQ(p.format, Format::Zip);
    EXPECT_EQ(ReroutedForEncryption(p, /*encrypted=*/true), Backend::LibArchive);
}

TEST(Reroute, RarStaysSevenZipDll) {
    auto p = PlanFromHeader(L"a.rar", std::string_view{});
    EXPECT_EQ(ReroutedForEncryption(p, /*encrypted=*/true), Backend::SevenZipDll);
    EXPECT_EQ(ReroutedForEncryption(p, /*encrypted=*/false), Backend::SevenZipDll);
}

// --- Disk-backed entry point -----------------------------------------------

TEST(DetectFile, ReadsRealAssetFromDisk) {
    const auto p = DetectFile(AssetPath("7z__single-file.7z").wstring());
    EXPECT_EQ(p.format, Format::SevenZip);
    EXPECT_EQ(p.kind, Kind::Container);
}

TEST(DetectFile, RenamedArchiveOverrideFromDisk) {
    const auto p =
        DetectFile(AssetPath("edge-renamed-7z-as-zip.zip").wstring());
    EXPECT_EQ(p.format, Format::SevenZip);
    EXPECT_TRUE(p.extensionOverriddenByContent);
}

TEST(DetectFile, MissingFileFallsBackToExtension) {
    const auto p = DetectFile(L"C:\\nope\\does-not-exist.tar.gz");
    EXPECT_EQ(p.format, Format::Tar);
    EXPECT_TRUE(HasFilter(p, Filter::Gzip));
    EXPECT_EQ(p.stem, L"does-not-exist");
}
