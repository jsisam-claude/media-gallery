#include "filmstrip.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "decoder.h"
#include "player.h"

namespace {

constexpr int kBaseHeight = 96; // at 96 dpi
constexpr int kPad = 6;
constexpr int kGap = 6;
constexpr size_t kCacheCap = 512;

// Scale a decoded image down into a device bitmap (fallback when the shell
// can't produce a thumbnail, e.g. exotic formats without a thumbnail handler).
HBITMAP ThumbFromDecoded(const DecodedImage& img, int maxSide) {
    if (img.width == 0 || img.height == 0) return nullptr;
    int w = (int)img.width, h = (int)img.height;
    double s = (std::min)((double)maxSide / w, (double)maxSide / h);
    if (s > 1.0) s = 1.0;
    int tw = (std::max)(1, (int)(w * s)), th = (std::max)(1, (int)(h * s));

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = tw;
    bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbmp) {
        HDC dc = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(dc, hbmp);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        BITMAPINFO srcBi = bi;
        srcBi.bmiHeader.biWidth = w;
        srcBi.bmiHeader.biHeight = -h;
        StretchDIBits(dc, 0, 0, tw, th, 0, 0, w, h, img.bgra.data(), &srcBi,
                      DIB_RGB_COLORS, SRCCOPY);
        SelectObject(dc, old);
        DeleteDC(dc);
    }
    ReleaseDC(nullptr, screen);
    return hbmp;
}

HBITMAP LoadShellThumb(const std::wstring& path, int px) {
    IShellItemImageFactory* f = nullptr;
    HBITMAP hbmp = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr,
                                              IID_IShellItemImageFactory,
                                              reinterpret_cast<void**>(&f)))) {
        SIZE sz{px, px};
        // THUMBNAILONLY: make GetImage fail when there is no real thumbnail
        // instead of substituting a generic file-type icon. Without it, on
        // machines where the shell thumbnail service is off (many VMs, or the
        // "Always show icons, never thumbnails" Explorer setting) every file
        // came back as an icon, so the decode-it-ourselves fallback below was
        // never reached.
        if (FAILED(f->GetImage(sz, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY,
                               &hbmp)))
            hbmp = nullptr;
        f->Release();
    }
    if (!hbmp) {
        if (ImageDecoder* d = FindDecoder(path)) {
            if (auto img = d->Load(path, 0)) hbmp = ThumbFromDecoded(*img, px);
        }
    }
    return hbmp;
}

// Real frame from the video engine (aspect-fit, so out dims may be smaller
// than px on one axis). Null on failure — including the image-only stub,
// whose player_extract_thumb always returns false — so callers fall back
// to the shell thumbnail.
HBITMAP ThumbFromVideo(const std::wstring& path, int px) {
    std::vector<uint8_t> buf((size_t)px * px * 4);
    int w = 0, h = 0;
    if (!player_extract_thumb(path.c_str(), px, px, buf.data(), &w, &h) ||
        w <= 0 || h <= 0)
        return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    // Both sides are tightly packed BGRA (32-bpp DIB rows have no padding).
    if (hbmp) memcpy(bits, buf.data(), (size_t)w * h * 4);
    return hbmp;
}

} // namespace

int Filmstrip::SizeClassFor(int cellPx) {
    for (int s : kSizes)
        if (cellPx <= s) return s;
    return kSizes[2];
}

void Filmstrip::Start(HWND owner, UINT readyMsg) {
    owner_ = owner;
    readyMsg_ = readyMsg;
    InitializeCriticalSection(&cs_);
    InitializeConditionVariable(&cv_);
    thread_ = CreateThread(nullptr, 0, &Filmstrip::ThreadProc, this, 0, nullptr);
}

void Filmstrip::Stop() {
    if (!thread_) return;
    EnterCriticalSection(&cs_);
    quit_ = true;
    LeaveCriticalSection(&cs_);
    WakeConditionVariable(&cv_);
    WaitForSingleObject(thread_, INFINITE);
    CloseHandle(thread_);
    thread_ = nullptr;
    DeleteCriticalSection(&cs_);
    for (auto& kv : cache_)
        if (kv.second) DeleteObject(kv.second);
    cache_.clear();
}

DWORD WINAPI Filmstrip::ThreadProc(void* self) {
    static_cast<Filmstrip*>(self)->WorkerLoop();
    return 0;
}

void Filmstrip::WorkerLoop() {
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    for (;;) {
        EnterCriticalSection(&cs_);
        while (queue_.empty() && !quit_)
            SleepConditionVariableCS(&cv_, &cs_, INFINITE);
        if (quit_) {
            LeaveCriticalSection(&cs_);
            break;
        }
        std::wstring path = std::move(queue_.front().path);
        int px = queue_.front().px;
        uint64_t gen = queue_.front().gen;
        queue_.pop_front();
        LeaveCriticalSection(&cs_);

        HBITMAP hbmp = IsVideoFile(path) ? ThumbFromVideo(path, px) : nullptr;
        if (!hbmp) hbmp = LoadShellThumb(path, px);
        auto* r = new ThumbResult{std::move(path), px, hbmp, gen};
        if (!PostMessageW(owner_, readyMsg_, 0, reinterpret_cast<LPARAM>(r))) {
            if (r->bmp) DeleteObject(r->bmp);
            delete r;
        }
    }
    if (SUCCEEDED(coInit)) CoUninitialize();
}

void Filmstrip::SetItems(const std::vector<std::wstring>* items) {
    items_ = items;
    scrollX_ = 0;
    current_ = -1;
}

void Filmstrip::SetCurrent(int index) {
    current_ = index;
    pendingEnsureVisible_ = true;
}

void Filmstrip::InvalidateThumb(const std::wstring& path) {
    ++gen_;  // any request already in flight for these keys is now stale
    for (int s : kSizes) {
        const std::wstring key = Key(path, s);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            if (it->second) DeleteObject(it->second);
            cache_.erase(it);
        }
        requested_.erase(key);
        invalidatedGen_[key] = gen_;
    }
}

void Filmstrip::OnThumbReady(ThumbResult* r) {
    const std::wstring key = Key(r->path, r->px);
    // Discard a thumbnail that was requested before the last invalidation of
    // this key: the file changed mid-generation, so this bitmap is outdated.
    auto inv = invalidatedGen_.find(key);
    if (inv != invalidatedGen_.end() && r->gen < inv->second) {
        if (r->bmp) DeleteObject(r->bmp);
        delete r;
        return;
    }
    EvictIfNeeded();
    auto it = cache_.find(key);
    if (it != cache_.end() && it->second) DeleteObject(it->second);
    cache_[key] = r->bmp; // may be null: remembered as failed, no re-request
    delete r;
}

HBITMAP Filmstrip::GetThumb(const std::wstring& path, int px) const {
    auto it = cache_.find(Key(path, px));
    return it != cache_.end() ? it->second : nullptr;
}

HBITMAP Filmstrip::GetThumbAny(const std::wstring& path) const {
    for (int i = 2; i >= 0; --i) { // prefer the largest loaded class
        auto it = cache_.find(Key(path, kSizes[i]));
        if (it != cache_.end() && it->second) return it->second;
    }
    return nullptr;
}

void Filmstrip::EvictIfNeeded() {
    if (cache_.size() < kCacheCap) return;
    for (auto& kv : cache_)
        if (kv.second) DeleteObject(kv.second);
    cache_.clear(); // thumbnails reload lazily; trivially correct eviction
    requested_.clear();
    invalidatedGen_.clear(); // keys are gone; their stale-guards are moot
}

void Filmstrip::Request(const std::wstring& path, int px) {
    const std::wstring key = Key(path, px);
    if (requested_.count(key)) return;
    requested_[key] = true;
    EnterCriticalSection(&cs_);
    queue_.push_back({path, px, gen_});
    LeaveCriticalSection(&cs_);
    WakeConditionVariable(&cv_);
}

int Filmstrip::Height(int dpi) const { return MulDiv(kBaseHeight, dpi, 96); }

// Pads and gaps scale with DPI like the strip height does; fixed-pixel spacing
// makes 96px-proportioned cells look crammed at 150/200% scaling.
int Filmstrip::Pad() const { return MulDiv(kPad, dpi_, 96); }
int Filmstrip::Gap() const { return MulDiv(kGap, dpi_, 96); }

void Filmstrip::Scroll(int wheelDelta) {
    scrollX_ -= wheelDelta; // clamped in Draw where the width is known
    if (scrollX_ < 0) scrollX_ = 0;
}

int Filmstrip::HitTest(POINT pt, const RECT& rc) const {
    if (!items_ || pt.y < rc.top || pt.y >= rc.bottom) return -1;
    const int gap = Gap();
    int cell = (rc.bottom - rc.top) - 2 * Pad();
    int x = pt.x - rc.left - gap + scrollX_;
    if (x < 0) return -1;
    int idx = x / (cell + gap);
    if (x % (cell + gap) >= cell) return -1; // in the gap
    return (idx >= 0 && idx < (int)items_->size()) ? idx : -1;
}

void Filmstrip::Draw(HDC dc, const RECT& rc) {
    HBRUSH bg = CreateSolidBrush(RGB(32, 32, 32));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    if (!items_ || items_->empty()) return;

    const int stripH = rc.bottom - rc.top;
    const int pad = Pad(), gap = Gap();
    const int cell = stripH - 2 * pad;
    const int step = cell + gap;
    const int viewW = rc.right - rc.left;
    const int n = (int)items_->size();
    const int contentW = gap + n * step;

    if (pendingEnsureVisible_ && current_ >= 0) {
        int cx = gap + current_ * step;
        if (cx - scrollX_ < 0) scrollX_ = cx - gap;
        else if (cx + cell - scrollX_ > viewW) scrollX_ = cx + cell - viewW + gap;
        pendingEnsureVisible_ = false;
    }
    int maxScroll = (std::max)(0, contentW - viewW);
    if (scrollX_ > maxScroll) scrollX_ = maxScroll;
    if (scrollX_ < 0) scrollX_ = 0;

    int first = (std::max)(0, (scrollX_ - gap) / step);
    int last = (std::min)(n - 1, (scrollX_ + viewW) / step);

    HDC mem = CreateCompatibleDC(dc);
    for (int i = first; i <= last; ++i) {
        const std::wstring& path = (*items_)[i];
        RECT cellRc{rc.left + gap + i * step - scrollX_, rc.top + pad, 0, 0};
        cellRc.right = cellRc.left + cell;
        cellRc.bottom = cellRc.top + cell;

        if (!GetThumb(path, kSizes[0]) &&
            !requested_.count(Key(path, kSizes[0])))
            Request(path, kSizes[0]);

        HBITMAP thumb = GetThumbAny(path);
        if (thumb) {
            BITMAP bm{};
            GetObjectW(thumb, sizeof(bm), &bm);
            // Letterbox: center the (aspect-preserved) thumbnail in the cell.
            double s = (std::min)((double)cell / bm.bmWidth, (double)cell / bm.bmHeight);
            int tw = (std::max)(1, (int)(bm.bmWidth * s));
            int th = (std::max)(1, (int)(bm.bmHeight * s));
            int tx = cellRc.left + (cell - tw) / 2;
            int ty = cellRc.top + (cell - th) / 2;
            HGDIOBJ old = SelectObject(mem, thumb);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, tx, ty, tw, th, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(mem, old);
        } else {
            HBRUSH ph = CreateSolidBrush(RGB(52, 52, 52));
            FillRect(dc, &cellRc, ph);
            DeleteObject(ph);
        }

        if (i == current_) {
            HBRUSH hl = CreateSolidBrush(RGB(0, 120, 215));
            RECT b = cellRc;
            InflateRect(&b, 3, 3);
            FrameRect(dc, &b, hl);
            InflateRect(&b, -1, -1);
            FrameRect(dc, &b, hl);
            DeleteObject(hl);
        }
    }
    DeleteDC(mem);
}
