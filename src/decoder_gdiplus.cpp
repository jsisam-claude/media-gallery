// GDI+ decoder: JPEG/GIF/PNG/BMP/TIFF/ICO via the OS-patched codecs.
// Also hosts the decoder registry (InitDecoders etc.).
#include "decoder.h"

#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>

#include <algorithm>

using namespace Gdiplus;

namespace {

ULONG_PTR g_gdiplusToken = 0;

std::wstring FormatRational(const PropertyItem& pi, bool asFraction) {
    if (pi.type != PropertyTagTypeRational || pi.length < 8 || !pi.value) return L"";
    const UINT32* v = static_cast<const UINT32*>(pi.value);
    UINT32 num = v[0], den = v[1];
    if (den == 0) return L"";
    wchar_t buf[64];
    if (asFraction && num != 0 && num < den) { // e.g. exposure 1/125
        swprintf(buf, 64, L"1/%u s", den / num);
    } else {
        double d = double(num) / double(den);
        swprintf(buf, 64, (d == (int)d) ? L"%.0f" : L"%.1f", d);
    }
    return buf;
}

class GdiplusDecoder final : public ImageDecoder {
public:
    const std::vector<std::wstring>& Extensions() const override {
        static const std::vector<std::wstring> exts = {
            L"jpg", L"jpeg", L"jpe", L"gif", L"png", L"bmp", L"dib",
            L"tif", L"tiff", L"ico",
        };
        return exts;
    }

    std::unique_ptr<DecodedImage> Load(const std::wstring& path, UINT page) const override {
        Bitmap bmp(path.c_str(), FALSE);
        if (bmp.GetLastStatus() != Ok) return nullptr;

        auto out = std::make_unique<DecodedImage>();
        ReadFrameInfo(bmp, *out);
        if (out->pageCount > 1 && page > 0) {
            GUID dim = FrameDimensionPage;
            bmp.SelectActiveFrame(&dim, (std::min)(page, out->pageCount - 1));
        }
        ReadMetadata(bmp, path, *out);

        // Enforce the decompression-bomb guard BEFORE ApplyExifOrientation:
        // RotateFlip forces a full decode and a second rotated allocation, so
        // a huge crafted image with an orientation tag would blow past the
        // pixel cap before the check if it ran afterward.
        {
            UINT w0 = bmp.GetWidth(), h0 = bmp.GetHeight();
            if (w0 == 0 || h0 == 0 || UINT64(w0) * h0 > kMaxPixels) return nullptr;
        }

        // Bake EXIF orientation into pixels so the app never worries about it.
        ApplyExifOrientation(bmp);

        UINT w = bmp.GetWidth(), h = bmp.GetHeight();
        if (w == 0 || h == 0 || UINT64(w) * h > kMaxPixels) return nullptr;

        BitmapData bd{};
        Rect rect(0, 0, (INT)w, (INT)h);
        if (bmp.LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok)
            return nullptr;
        out->width = w;
        out->height = h;
        out->bgra.resize(size_t(w) * h * 4);
        for (UINT y = 0; y < h; ++y) {
            memcpy(&out->bgra[size_t(y) * w * 4],
                   static_cast<const BYTE*>(bd.Scan0) + size_t(y) * bd.Stride,
                   size_t(w) * 4);
        }
        bmp.UnlockBits(&bd);
        return out;
    }

    bool CanSaveRotation(const std::wstring& path, const DecodedImage& img) const override {
        if (img.multiFrame) return false; // would flatten animation / extra pages
        const std::wstring e = FileExtLower(path);
        return e == L"jpg" || e == L"jpeg" || e == L"jpe" || e == L"png" ||
               e == L"bmp" || e == L"dib" || e == L"gif" || e == L"tif" || e == L"tiff";
    }

    bool SaveRotation(const std::wstring& path, int quarterTurns) const override {
        quarterTurns = ((quarterTurns % 4) + 4) % 4;
        if (quarterTurns == 0) return true;

        const std::wstring tmp = path + L".pgtmp";
        const std::wstring ext = FileExtLower(path);
        bool ok = false;
        if (ext == L"jpg" || ext == L"jpeg" || ext == L"jpe")
            ok = SaveJpegLossless(path, tmp, quarterTurns);
        if (!ok)
            ok = SaveReencoded(path, tmp, quarterTurns);
        if (!ok) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        if (!ReplaceFileW(path.c_str(), tmp.c_str(), nullptr,
                          REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) &&
            !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

private:
    static void ReadFrameInfo(Bitmap& bmp, DecodedImage& out) {
        UINT dims = bmp.GetFrameDimensionsCount();
        if (dims == 0) return;
        std::vector<GUID> ids(dims);
        if (bmp.GetFrameDimensionsList(ids.data(), dims) != Ok) return;
        for (UINT i = 0; i < dims; ++i) {
            UINT count = bmp.GetFrameCount(&ids[i]);
            if (count > 1) out.multiFrame = true;
            if (ids[i] == FrameDimensionPage && count > 1) out.pageCount = count;
        }
    }

    static int ReadOrientation(Image& img) {
        UINT size = img.GetPropertyItemSize(PropertyTagOrientation);
        if (size < sizeof(PropertyItem)) return 1;
        std::vector<BYTE> buf(size);
        PropertyItem* pi = reinterpret_cast<PropertyItem*>(buf.data());
        if (img.GetPropertyItem(PropertyTagOrientation, size, pi) != Ok) return 1;
        if (pi->type != PropertyTagTypeShort || pi->length < 2 || !pi->value) return 1;
        int o = *static_cast<const UINT16*>(pi->value);
        return (o >= 1 && o <= 8) ? o : 1;
    }

    static void ApplyExifOrientation(Bitmap& bmp) {
        static const RotateFlipType kMap[9] = {
            RotateNoneFlipNone, RotateNoneFlipNone, RotateNoneFlipX,
            Rotate180FlipNone,  RotateNoneFlipY,    Rotate90FlipX,
            Rotate90FlipNone,   Rotate270FlipX,     Rotate270FlipNone,
        };
        int o = ReadOrientation(bmp);
        if (o > 1) bmp.RotateFlip(kMap[o]);
    }

    static void AddMetaString(Image& img, PROPID id, const wchar_t* label, DecodedImage& out) {
        UINT size = img.GetPropertyItemSize(id);
        if (size < sizeof(PropertyItem)) return;
        std::vector<BYTE> buf(size);
        PropertyItem* pi = reinterpret_cast<PropertyItem*>(buf.data());
        if (img.GetPropertyItem(id, size, pi) != Ok) return;
        if (pi->type != PropertyTagTypeASCII || pi->length == 0 || !pi->value) return;
        const char* s = static_cast<const char*>(pi->value);
        // Bound the conversion by the item's declared value length, NOT a -1
        // NUL scan: a crafted EXIF ASCII value that omits the trailing NUL
        // would make MultiByteToWideChar read past the property buffer (heap
        // over-read shown in the details pane).
        int n = (int)pi->length;
        int wlen = MultiByteToWideChar(CP_ACP, 0, s, n, nullptr, 0);
        if (wlen <= 0) return;
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, s, n, &w[0], wlen);
        while (!w.empty() && (w.back() == L' ' || w.back() == L'\0')) w.pop_back();
        if (!w.empty()) out.meta.emplace_back(label, w);
    }

    static bool GetProp(Image& img, PROPID id, std::vector<BYTE>& buf) {
        UINT size = img.GetPropertyItemSize(id);
        if (size < sizeof(PropertyItem)) return false;
        buf.resize(size);
        return img.GetPropertyItem(id, size, reinterpret_cast<PropertyItem*>(buf.data())) == Ok;
    }

    static void ReadMetadata(Bitmap& bmp, const std::wstring& path, DecodedImage& out) {
        std::wstring fmt = FileExtLower(path);
        std::transform(fmt.begin(), fmt.end(), fmt.begin(), ::towupper);
        out.meta.emplace_back(L"Format", fmt);
        {
            wchar_t buf[16];
            swprintf(buf, 16, L"%u-bit",
                     (unsigned)((bmp.GetPixelFormat() >> 8) & 0xff));
            out.meta.emplace_back(L"Bit depth", buf);
        }
        std::vector<BYTE> buf;
        if (GetProp(bmp, PropertyTagExifDTOrig, buf)) { // "YYYY:MM:DD HH:MM:SS"
            PropertyItem* pi = reinterpret_cast<PropertyItem*>(buf.data());
            if (pi->type == PropertyTagTypeASCII && pi->length >= 19 && pi->value) {
                std::string s(static_cast<const char*>(pi->value), 19);
                if (s.size() == 19) { s[4] = '-'; s[7] = '-'; }
                std::wstring w(s.begin(), s.end());
                out.meta.emplace_back(L"Date taken", w);
            }
        }
        AddMetaString(bmp, PropertyTagEquipMake, L"Camera make", out);
        AddMetaString(bmp, PropertyTagEquipModel, L"Camera model", out);
        if (GetProp(bmp, PropertyTagExifExposureTime, buf)) {
            auto v = FormatRational(*reinterpret_cast<PropertyItem*>(buf.data()), true);
            if (!v.empty()) out.meta.emplace_back(L"Exposure time", v);
        }
        if (GetProp(bmp, PropertyTagExifFNumber, buf)) {
            auto v = FormatRational(*reinterpret_cast<PropertyItem*>(buf.data()), false);
            if (!v.empty()) out.meta.emplace_back(L"F-number", L"f/" + v);
        }
        if (GetProp(bmp, PropertyTagExifISOSpeed, buf)) {
            PropertyItem* pi = reinterpret_cast<PropertyItem*>(buf.data());
            if (pi->type == PropertyTagTypeShort && pi->length >= 2 && pi->value) {
                wchar_t b[32];
                swprintf(b, 32, L"ISO-%u", (unsigned)*static_cast<const UINT16*>(pi->value));
                out.meta.emplace_back(L"ISO speed", b);
            }
        }
        if (GetProp(bmp, PropertyTagExifFocalLength, buf)) {
            auto v = FormatRational(*reinterpret_cast<PropertyItem*>(buf.data()), false);
            if (!v.empty()) out.meta.emplace_back(L"Focal length", v + L" mm");
        }
    }

    static bool GetEncoderClsid(const wchar_t* mime, CLSID& clsid) {
        UINT num = 0, size = 0;
        if (GetImageEncodersSize(&num, &size) != Ok || size == 0) return false;
        std::vector<BYTE> buf(size);
        ImageCodecInfo* info = reinterpret_cast<ImageCodecInfo*>(buf.data());
        if (GetImageEncoders(num, size, info) != Ok) return false;
        for (UINT i = 0; i < num; ++i) {
            if (wcscmp(info[i].MimeType, mime) == 0) {
                clsid = info[i].Clsid;
                return true;
            }
        }
        return false;
    }

    // Lossless JPEG rotation. The EXIF orientation tag (if any) is left as-is:
    // rotating the stored pixels by `q` (or -q for mirrored tags) composes with
    // the tag to give exactly the on-screen result.
    static bool SaveJpegLossless(const std::wstring& src, const std::wstring& tmp, int q) {
        Image img(src.c_str());
        if (img.GetLastStatus() != Ok) return false;
        int o = ReadOrientation(img);
        bool mirrored = (o == 2 || o == 4 || o == 5 || o == 7);
        int t = mirrored ? (4 - q) % 4 : q;
        if (t == 0) return true;
        static const EncoderValue kRot[4] = {
            EncoderValueTransformRotate90, EncoderValueTransformRotate90,
            EncoderValueTransformRotate180, EncoderValueTransformRotate270,
        };
        CLSID clsid;
        if (!GetEncoderClsid(L"image/jpeg", clsid)) return false;
        EncoderParameters params{};
        ULONG value = kRot[t];
        params.Count = 1;
        params.Parameter[0].Guid = EncoderTransformation;
        params.Parameter[0].Type = EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        params.Parameter[0].Value = &value;
        return img.Save(tmp.c_str(), &clsid, &params) == Ok;
    }

    static bool SaveReencoded(const std::wstring& src, const std::wstring& tmp, int q) {
        Bitmap bmp(src.c_str(), FALSE);
        if (bmp.GetLastStatus() != Ok) return false;
        ApplyExifOrientation(bmp);
        static const RotateFlipType kRot[4] = {
            RotateNoneFlipNone, Rotate90FlipNone, Rotate180FlipNone, Rotate270FlipNone,
        };
        if (q != 0 && bmp.RotateFlip(kRot[q]) != Ok) return false;
        // Orientation is now baked in; neutralize the tag so it isn't applied twice.
        if (ReadOrientation(bmp) > 1) {
            UINT16 one = 1;
            PropertyItem pi{};
            pi.id = PropertyTagOrientation;
            pi.type = PropertyTagTypeShort;
            pi.length = sizeof(one);
            pi.value = &one;
            bmp.SetPropertyItem(&pi);
        }
        const std::wstring ext = FileExtLower(src);
        const wchar_t* mime = L"image/png";
        if (ext == L"jpg" || ext == L"jpeg" || ext == L"jpe") mime = L"image/jpeg";
        else if (ext == L"bmp" || ext == L"dib") mime = L"image/bmp";
        else if (ext == L"gif") mime = L"image/gif";
        else if (ext == L"tif" || ext == L"tiff") mime = L"image/tiff";
        CLSID clsid;
        if (!GetEncoderClsid(mime, clsid)) return false;
        if (wcscmp(mime, L"image/jpeg") == 0) {
            EncoderParameters params{};
            ULONG quality = 95;
            params.Count = 1;
            params.Parameter[0].Guid = EncoderQuality;
            params.Parameter[0].Type = EncoderParameterValueTypeLong;
            params.Parameter[0].NumberOfValues = 1;
            params.Parameter[0].Value = &quality;
            return bmp.Save(tmp.c_str(), &clsid, &params) == Ok;
        }
        return bmp.Save(tmp.c_str(), &clsid, nullptr) == Ok;
    }
};

std::vector<ImageDecoder*> g_decoders;

} // namespace

// Defined in decoder_wic.cpp; returns nullptr if WIC is unavailable.
ImageDecoder* CreateWicDecoder();

void InitDecoders() {
    if (!g_decoders.empty()) return;
    GdiplusStartupInput gsi;
    if (GdiplusStartup(&g_gdiplusToken, &gsi, nullptr) != Ok) g_gdiplusToken = 0;
    g_decoders.push_back(new GdiplusDecoder());
    if (ImageDecoder* wic = CreateWicDecoder()) g_decoders.push_back(wic);
}

void ShutdownDecoders() {
    for (ImageDecoder* d : g_decoders) delete d;
    g_decoders.clear();
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

const std::vector<ImageDecoder*>& Decoders() { return g_decoders; }

std::wstring FileExtLower(const std::wstring& path) {
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    if (!ext || !*ext) return L"";
    std::wstring e(ext + 1);
    std::transform(e.begin(), e.end(), e.begin(), ::towlower);
    return e;
}

ImageDecoder* FindDecoder(const std::wstring& path) {
    const std::wstring ext = FileExtLower(path);
    if (ext.empty()) return nullptr;
    for (ImageDecoder* d : g_decoders) {
        const auto& exts = d->Extensions();
        if (std::find(exts.begin(), exts.end(), ext) != exts.end()) return d;
    }
    return nullptr;
}

bool IsSupportedImage(const std::wstring& path) { return FindDecoder(path) != nullptr; }

// Video containers handled by the optional player engine (player.h). Whether
// the engine is actually available is the app's call, not this predicate's.
static const wchar_t* const kVideoExts[] = {L"mp4", L"m4v", L"mov",
                                            L"mkv", L"webm", L"avi"};

bool IsVideoFile(const std::wstring& path) {
    const std::wstring ext = FileExtLower(path);
    for (const wchar_t* e : kVideoExts)
        if (ext == e) return true;
    return false;
}

std::wstring OpenDialogFilter(bool includeVideo) {
    std::wstring patterns;
    for (ImageDecoder* d : g_decoders) {
        for (const auto& e : d->Extensions()) {
            std::wstring pat = L"*." + e;
            if (patterns.find(pat) == std::wstring::npos) {
                if (!patterns.empty()) patterns += L";";
                patterns += pat;
            }
        }
    }
    if (includeVideo) {
        for (const wchar_t* e : kVideoExts) {
            std::wstring pat = std::wstring(L"*.") + e;
            if (patterns.find(pat) == std::wstring::npos) {
                if (!patterns.empty()) patterns += L";";
                patterns += pat;
            }
        }
    }
    std::wstring filter = includeVideo ? L"Images & videos" : L"Images";
    filter += L'\0';
    filter += patterns;
    filter += L'\0';
    filter += L"All files";
    filter += L'\0';
    filter += L"*.*";
    filter += L'\0';
    return filter;
}
