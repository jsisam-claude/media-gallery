// Thumbnail provider + bottom filmstrip ribbon.
// Shell-provided thumbnails (Explorer's own pipeline) loaded on a background
// thread in three size classes (256/768/1536 px) — shared by the filmstrip,
// the grid view and the viewer's instant-preview.
#pragma once
#include <windows.h>

#include <deque>
#include <list>
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
    void SetDpi(int dpi) { dpi_ = dpi; }                   // scales pads/gaps
    void InvalidateThumb(const std::wstring& path);        // file changed on disk

    struct ThumbResult {
        std::wstring path;
        int px;
        unsigned gen; // generation at request time; stale results are dropped
        HBITMAP bmp;  // may be null (load failed)
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
    int Pad() const;  // strip padding at the current DPI
    int Gap() const;  // inter-cell gap at the current DPI
    struct Entry {
        HBITMAP bmp = nullptr; // null = load failed (remembered, not re-requested)
        size_t bytes = 0;
        std::list<std::wstring>::iterator lruIt;
    };
    void TouchLru(const std::wstring& key, Entry& e) const;
    void EvictEntry(const std::wstring& key);
    void EvictIfNeeded();
    static DWORD WINAPI ThreadProc(void* self);
    void WorkerLoop();
    static std::wstring Key(const std::wstring& path, int px) {
        return path + L"|" + std::to_wstring(px);
    }

    const std::vector<std::wstring>* items_ = nullptr;
    int current_ = -1;
    int dpi_ = 96;
    int scrollX_ = 0;
    bool pendingEnsureVisible_ = false;

    // LRU cache bounded by bytes (and entry count as a backstop). mutable so
    // const lookups can refresh recency; all access is UI-thread only.
    mutable std::unordered_map<std::wstring, Entry> cache_;
    mutable std::list<std::wstring> lru_; // front = oldest
    size_t cacheBytes_ = 0;
    std::unordered_map<std::wstring, bool> requested_;
    unsigned gen_ = 0; // bumped by InvalidateThumb; stale worker results dropped

    struct Job {
        std::wstring path;
        int px;
        unsigned gen;
    };
    HWND owner_ = nullptr;
    UINT readyMsg_ = 0;
    HANDLE thread_ = nullptr;
    CRITICAL_SECTION cs_{};
    CONDITION_VARIABLE cv_{};
    std::deque<Job> queue_;
    bool quit_ = false;
};
