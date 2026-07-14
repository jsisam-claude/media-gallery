// Generic WIC decoder: enumerates the OS's installed imaging codecs at runtime,
// so anything Windows can decode (JPEG XR, DDS out of the box; HEIC/AVIF/WebP/RAW
// once the free Store extensions are installed) works with zero code changes.
// Registered after the GDI+ decoder, which keeps the classic formats.
#include "decoder.h"

#include <wincodec.h>

#include <algorithm>

namespace {

class WicDecoder final : public ImageDecoder {
public:
    explicit WicDecoder(IWICImagingFactory* factory) : factory_(factory) {
        EnumerateExtensions();
    }
    ~WicDecoder() override {
        if (factory_) factory_->Release();
    }
    WicDecoder(const WicDecoder&) = delete;
    WicDecoder& operator=(const WicDecoder&) = delete;

    bool Usable() const { return !extensions_.empty(); }

    const std::vector<std::wstring>& Extensions() const override { return extensions_; }

    std::unique_ptr<DecodedImage> Load(const std::wstring& path, UINT page) const override {
        IWICBitmapDecoder* dec = nullptr;
        if (FAILED(factory_->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &dec)))
            return nullptr;

        auto out = std::make_unique<DecodedImage>();
        UINT frames = 0;
        dec->GetFrameCount(&frames);
        if (frames == 0) {
            dec->Release();
            return nullptr;
        }
        out->pageCount = frames;
        out->multiFrame = frames > 1;
        AddFormatMeta(dec, *out);

        IWICBitmapFrameDecode* frame = nullptr;
        HRESULT hr = dec->GetFrame((std::min)(page, frames - 1), &frame);
        dec->Release();
        if (FAILED(hr)) return nullptr;

        IWICBitmapSource* src = nullptr;
        hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame, &src);
        frame->Release();
        if (FAILED(hr)) return nullptr;

        UINT w = 0, h = 0;
        src->GetSize(&w, &h);
        if (w == 0 || h == 0 || UINT64(w) * h > kMaxPixels) {
            src->Release();
            return nullptr;
        }
        out->width = w;
        out->height = h;
        out->bgra.resize(size_t(w) * h * 4);
        hr = src->CopyPixels(nullptr, w * 4, (UINT)out->bgra.size(), out->bgra.data());
        src->Release();
        if (FAILED(hr)) return nullptr;
        return out;
    }

private:
    void EnumerateExtensions() {
        IEnumUnknown* en = nullptr;
        if (FAILED(factory_->CreateComponentEnumerator(
                WICDecoder, WICComponentEnumerateDefault, &en)))
            return;
        IUnknown* unk = nullptr;
        ULONG got = 0;
        while (en->Next(1, &unk, &got) == S_OK && got == 1) {
            IWICBitmapDecoderInfo* info = nullptr;
            if (SUCCEEDED(unk->QueryInterface(IID_IWICBitmapDecoderInfo,
                                              reinterpret_cast<void**>(&info)))) {
                UINT len = 0;
                info->GetFileExtensions(0, nullptr, &len);
                if (len > 1) {
                    std::wstring exts(len, L'\0');
                    if (SUCCEEDED(info->GetFileExtensions(len, &exts[0], &len))) {
                        exts.resize(wcslen(exts.c_str()));
                        AddExtensionList(exts);
                    }
                }
                info->Release();
            }
            unk->Release();
        }
        en->Release();
    }

    void AddExtensionList(const std::wstring& commaList) { // ".jpg,.jpeg,..."
        size_t pos = 0;
        while (pos < commaList.size()) {
            size_t comma = commaList.find(L',', pos);
            if (comma == std::wstring::npos) comma = commaList.size();
            std::wstring e = commaList.substr(pos, comma - pos);
            pos = comma + 1;
            while (!e.empty() && (e.front() == L'.' || e.front() == L' ')) e.erase(0, 1);
            std::transform(e.begin(), e.end(), e.begin(), ::towlower);
            if (!e.empty() &&
                std::find(extensions_.begin(), extensions_.end(), e) == extensions_.end())
                extensions_.push_back(e);
        }
    }

    static void AddFormatMeta(IWICBitmapDecoder* dec, DecodedImage& out) {
        IWICBitmapDecoderInfo* info = nullptr;
        if (FAILED(dec->GetDecoderInfo(&info))) return;
        UINT len = 0;
        info->GetFriendlyName(0, nullptr, &len);
        if (len > 1) {
            std::wstring name(len, L'\0');
            if (SUCCEEDED(info->GetFriendlyName(len, &name[0], &len))) {
                name.resize(wcslen(name.c_str()));
                out.meta.emplace_back(L"Format", name);
            }
        }
        info->Release();
    }

    IWICImagingFactory* factory_;
    std::vector<std::wstring> extensions_;
};

} // namespace

ImageDecoder* CreateWicDecoder() {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory,
                                reinterpret_cast<void**>(&factory))))
        return nullptr;
    WicDecoder* d = new WicDecoder(factory); // takes ownership of factory
    if (!d->Usable()) {
        delete d;
        return nullptr;
    }
    return d;
}
