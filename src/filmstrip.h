// Thumbnail provider + bottom filmstrip ribbon.
// Shell-provided thumbnails (Explorer's own pipeline) loaded on a background
// thread in three size classes (256/768/1536 px) — shared by the filmstrip,
// the grid view and the viewer's instant-preview.
#pragma once
#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Filmstrip {
public:
    // readyMsg is posted to `owner` (lParam = ThumbResult*) when a thumbnail loads.
    void Start(HWND owner, UINT readyMsg);
    void Stop();

    void SetItems(const std::vector<std::wstring>* items); // owned by the app
    void SetCurrent(int index);                            // scrolls it into view
    void InvalidateThumb(const std::wstring& path);        // file changed on disk

    struct ThumbResult {
        std::wstring path;
        int px;
        HBITMAP bmp;   // may be null (load failed)
        uint64_t gen;  // request epoch; a result older than the key's last
                       // invalidation is discarded instead of re-cached
    };
    void OnThumbReady(ThumbResult* r); // takes ownership; call from UI thread

    // Async request + lookup. px must be one of kSizes. GetThumbAny returns the
    // largest size class already loaded (any px), so callers can show something
    // sharp-ish immediately while the exact size arrives.
    static int SizeClassFor(int cellPx); // smallest class >= cellPx (clamped)
    void Request(const std::wstring& path, int px);
    HBITMAP GetThumb(const std::wstring& path, int px) const;
    HBITMAP GetThumbAny(const std::wstring& path) const;

    int  Height(int dpi) const;
    void Draw(HDC dc, const RECT& rc); // cell size derives from rc height
    int  HitTest(POINT pt, const RECT& rc) const; // item index or -1
    void Scroll(int wheelDelta);

    static constexpr int kSizes[3] = {256, 768, 1536};

private:
    void EvictIfNeeded();
    static DWORD WINAPI ThreadProc(void* self);
    void WorkerLoop();
    static std::wstring Key(const std::wstring& path, int px) {
        return path + L"|" + std::to_wstring(px);
    }

    const std::vector<std::wstring>* items_ = nullptr;
    int current_ = -1;
    int scrollX_ = 0;
    bool pendingEnsureVisible_ = false;

    std::unordered_map<std::wstring, HBITMAP> cache_; // null value = failed load
    std::unordered_map<std::wstring, bool> requested_;
    // Per-key epoch of the last InvalidateThumb; a result whose request epoch
    // is older is stale (the file changed mid-generation) and is discarded.
    std::unordered_map<std::wstring, uint64_t> invalidatedGen_;
    uint64_t gen_ = 1; // monotonic request epoch (UI thread only)

    HWND owner_ = nullptr;
    UINT readyMsg_ = 0;
    HANDLE thread_ = nullptr;
    CRITICAL_SECTION cs_{};
    CONDITION_VARIABLE cv_{};
    struct QueuedThumb { std::wstring path; int px; uint64_t gen; };
    std::deque<QueuedThumb> queue_;
    bool quit_ = false;
};
