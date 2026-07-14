// Bottom thumbnail ribbon: shell-provided thumbnails (Explorer's own pipeline),
// loaded on a background thread, click-to-jump, auto-scroll, wheel scroll.
#pragma once
#include <windows.h>

#include <deque>
#include <string>
#include <unordered_map>
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
        HBITMAP bmp; // may be null (load failed)
    };
    void OnThumbReady(ThumbResult* r); // takes ownership; call from UI thread

    int  Height(int dpi) const;
    void Draw(HDC dc, const RECT& rc); // cell size derives from rc height
    int  HitTest(POINT pt, const RECT& rc) const; // item index or -1
    void Scroll(int wheelDelta);

    // Full-size preview source while the real decode is in flight (may be null).
    HBITMAP GetThumb(const std::wstring& path) const;

private:
    void Request(const std::wstring& path);
    void EvictIfNeeded();
    static DWORD WINAPI ThreadProc(void* self);
    void WorkerLoop();

    const std::vector<std::wstring>* items_ = nullptr;
    int current_ = -1;
    int scrollX_ = 0;
    bool pendingEnsureVisible_ = false;

    std::unordered_map<std::wstring, HBITMAP> cache_; // null value = failed load
    std::unordered_map<std::wstring, bool> requested_;

    HWND owner_ = nullptr;
    UINT readyMsg_ = 0;
    HANDLE thread_ = nullptr;
    CRITICAL_SECTION cs_{};
    CONDITION_VARIABLE cv_{};
    std::deque<std::wstring> queue_;
    bool quit_ = false;
};
