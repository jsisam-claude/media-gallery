// Modular image decoding — format-agnostic contract between the app and codecs.
// Adding a format = one new decoder_*.cpp registered in InitDecoders(); the
// extension allowlist, open-dialog filter and folder scanning all derive from it.
#pragma once
#include <windows.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

struct DecodedImage {
    UINT width = 0;
    UINT height = 0;
    UINT pageCount = 1;      // navigable pages (multi-page TIFF etc.)
    bool multiFrame = false; // >1 frame of any kind (incl. GIF animation)
    std::vector<BYTE> bgra;  // 32-bpp straight BGRA, top-down, stride = width*4
    std::vector<std::pair<std::wstring, std::wstring>> meta; // label/value for details pane
};

// Decompression-bomb guard shared by all decoders (~536 MB of pixels).
constexpr UINT64 kMaxPixels = 134u * 1000 * 1000;

class ImageDecoder {
public:
    virtual ~ImageDecoder() = default;
    virtual const std::vector<std::wstring>& Extensions() const = 0; // lowercase, no dot
    virtual std::unique_ptr<DecodedImage> Load(const std::wstring& path, UINT page) const = 0;
    // Persisting a rotation is an optional capability (quarterTurns 1..3, clockwise).
    virtual bool CanSaveRotation(const std::wstring&, const DecodedImage&) const { return false; }
    virtual bool SaveRotation(const std::wstring&, int) const { return false; }
};

// Call once after CoInitializeEx on the main thread; idempotent.
void InitDecoders();
void ShutdownDecoders();

const std::vector<ImageDecoder*>& Decoders();
ImageDecoder* FindDecoder(const std::wstring& path); // by extension, nullptr if none
bool IsSupportedImage(const std::wstring& path);
std::wstring FileExtLower(const std::wstring& path); // "jpg" (no dot), lowercase
std::wstring OpenDialogFilter();                     // GetOpenFileName filter (embedded NULs)
