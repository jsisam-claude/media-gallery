// Media Gallery — minimal, secure Windows photo viewer (WinAPI + modular decoders).
// See README.md for keys and design notes.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "decoder.h"
#include "filmstrip.h"
#include "player.h"
#include "resource.h"

#define WM_APP_DECODED (WM_APP + 1)
#define WM_APP_THUMB   (WM_APP + 2)
#define WM_APP_LIST    (WM_APP + 3)
#define WM_APP_SAVED   (WM_APP + 4)
#define WM_APP_PLAYER  (WM_APP + 5) // wParam = PlayerEvent, lParam = open generation
#define WM_APP_PROBED  (WM_APP + 6)

namespace {

constexpr COLORREF kBg = RGB(30, 30, 30);
constexpr COLORREF kPanelBg = RGB(24, 24, 24);
constexpr COLORREF kPanelBorder = RGB(80, 80, 80);
constexpr COLORREF kText = RGB(235, 235, 235);
constexpr COLORREF kTextDim = RGB(160, 160, 160);
constexpr COLORREF kAccent = RGB(0, 120, 215);
constexpr double kMinZoom = 0.05, kMaxZoom = 16.0;
constexpr UINT_PTR kTimerHq = 1;
constexpr UINT_PTR kTimerVideo = 2; // transport-bar refresh while a video plays
constexpr UINT_PTR kTimerSlideshow = 3;
constexpr UINT kSlideshowMs = 5000; // default per-image dwell (View > Slideshow Options)
constexpr int kVideoBarH = 34;      // transport bar height, pre-DPI-scale

// ---------------------------------------------------------------- decode worker

struct DecodeDone {
    std::wstring path;
    UINT page;
    DecodedImage* img; // null = decode failed
};
struct SaveDone {
    std::wstring path;
    bool ok;
};
struct ProbeDone {
    std::wstring path;
    bool ok;
    PlayerMediaInfo info; // video metadata for the details pane
};

class DecodeWorker {
public:
    void Start(HWND owner) {
        owner_ = owner;
        InitializeCriticalSection(&cs_);
        InitializeConditionVariable(&cv_);
        thread_ = CreateThread(nullptr, 0, &DecodeWorker::ThreadProc, this, 0, nullptr);
    }
    void Stop() {
        if (!thread_) return;
        EnterCriticalSection(&cs_);
        quit_ = true;
        LeaveCriticalSection(&cs_);
        WakeConditionVariable(&cv_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
        DeleteCriticalSection(&cs_);
    }
    // Most-recent-wins: drops queued decodes and probes (keeps queued saves).
    void RequestCurrent(const std::wstring& path, UINT page) {
        EnterCriticalSection(&cs_);
        q_.erase(std::remove_if(q_.begin(), q_.end(),
                                [](const Job& j) { return j.kind != Job::Save; }),
                 q_.end());
        q_.push_front({Job::Decode, path, page, 0});
        LeaveCriticalSection(&cs_);
        WakeConditionVariable(&cv_);
    }
    // Video metadata for the details pane (player_probe hits the disk).
    void RequestProbe(const std::wstring& path) {
        EnterCriticalSection(&cs_);
        q_.push_front({Job::Probe, path, 0, 0});
        LeaveCriticalSection(&cs_);
        WakeConditionVariable(&cv_);
    }
    void RequestPrefetch(const std::wstring& path, UINT page) {
        EnterCriticalSection(&cs_);
        q_.push_back({Job::Decode, path, page, 0});
        LeaveCriticalSection(&cs_);
        WakeConditionVariable(&cv_);
    }
    void RequestSave(const std::wstring& path, int quarter) {
        EnterCriticalSection(&cs_);
        q_.push_back({Job::Save, path, 0, quarter});
        LeaveCriticalSection(&cs_);
        WakeConditionVariable(&cv_);
    }

private:
    struct Job {
        enum Kind { Decode, Save, Probe } kind;
        std::wstring path;
        UINT page;
        int quarter;
    };
    static DWORD WINAPI ThreadProc(void* self) {
        static_cast<DecodeWorker*>(self)->Loop();
        return 0;
    }
    void Loop() {
        HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            EnterCriticalSection(&cs_);
            while (q_.empty() && !quit_) SleepConditionVariableCS(&cv_, &cs_, INFINITE);
            if (quit_) {
                LeaveCriticalSection(&cs_);
                break;
            }
            Job job = std::move(q_.front());
            q_.pop_front();
            LeaveCriticalSection(&cs_);

            if (job.kind == Job::Probe) {
                auto* d = new ProbeDone{std::move(job.path), false, PlayerMediaInfo{}};
                d->ok = player_probe(d->path.c_str(), &d->info);
                if (!PostMessageW(owner_, WM_APP_PROBED, 0, reinterpret_cast<LPARAM>(d)))
                    delete d;
                continue;
            }
            ImageDecoder* dec = FindDecoder(job.path);
            if (job.kind == Job::Decode) {
                DecodedImage* img = nullptr;
                if (dec) img = dec->Load(job.path, job.page).release();
                auto* d = new DecodeDone{std::move(job.path), job.page, img};
                if (!PostMessageW(owner_, WM_APP_DECODED, 0, reinterpret_cast<LPARAM>(d))) {
                    delete d->img;
                    delete d;
                }
            } else {
                bool ok = dec && dec->SaveRotation(job.path, job.quarter);
                auto* d = new SaveDone{std::move(job.path), ok};
                if (!PostMessageW(owner_, WM_APP_SAVED, 0, reinterpret_cast<LPARAM>(d)))
                    delete d;
            }
        }
        if (SUCCEEDED(coInit)) CoUninitialize();
    }

    HWND owner_ = nullptr;
    HANDLE thread_ = nullptr;
    CRITICAL_SECTION cs_{};
    CONDITION_VARIABLE cv_{};
    std::deque<Job> q_;
    bool quit_ = false;
};

// ------------------------------------------------- folder list (Explorer order)

struct ListDone {
    unsigned gen;
    std::vector<std::wstring> files;
    int start;
};
struct ListReq {
    HWND hwnd;
    unsigned gen;
    std::wstring target; // file or folder (canonical)
    bool isFolder;
};

std::wstring ParentDir(const std::wstring& path) {
    // No fixed MAX_PATH buffer: a longer path would come back truncated, the
    // folder scan would fail, and the file would open with no prev/next.
    size_t cut = path.find_last_of(L'\\');
    if (cut == std::wstring::npos) return L"";
    if (cut == 2 && path[1] == L':') cut = 3; // keep the root slash of "C:\x"
    return path.substr(0, cut);
}

bool IsPlayableVideo(const std::wstring& path); // fwd decl (needs app state, defined below)

// Items of an open Explorer window showing `dir`, in its current display order.
std::vector<std::wstring> ExplorerViewOrder(const std::wstring& dir) {
    std::vector<std::wstring> out;
    IShellWindows* sw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                IID_IShellWindows, reinterpret_cast<void**>(&sw))))
        return out;
    long count = 0;
    sw->get_Count(&count);
    for (long i = 0; i < count && out.empty(); ++i) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = i;
        IDispatch* disp = nullptr;
        if (sw->Item(v, &disp) != S_OK || !disp) continue;

        IServiceProvider* sp = nullptr;
        IShellBrowser* sb = nullptr;
        IShellView* sv = nullptr;
        IFolderView* fv = nullptr;
        IPersistFolder2* pf = nullptr;
        PIDLIST_ABSOLUTE folderPidl = nullptr;

        if (SUCCEEDED(disp->QueryInterface(IID_IServiceProvider,
                                           reinterpret_cast<void**>(&sp))) &&
            SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser,
                                       reinterpret_cast<void**>(&sb))) &&
            SUCCEEDED(sb->QueryActiveShellView(&sv)) &&
            SUCCEEDED(sv->QueryInterface(IID_IFolderView,
                                         reinterpret_cast<void**>(&fv))) &&
            SUCCEEDED(fv->GetFolder(IID_IPersistFolder2,
                                    reinterpret_cast<void**>(&pf))) &&
            SUCCEEDED(pf->GetCurFolder(&folderPidl))) {
            wchar_t folderPath[MAX_PATH];
            if (SHGetPathFromIDListW(folderPidl, folderPath) &&
                lstrcmpiW(folderPath, dir.c_str()) == 0) {
                int n = 0;
                if (SUCCEEDED(fv->ItemCount(SVGIO_ALLVIEW, &n))) {
                    for (int j = 0; j < n; ++j) {
                        PITEMID_CHILD child = nullptr;
                        if (fv->Item(j, &child) != S_OK || !child) continue;
                        PIDLIST_ABSOLUTE full = ILCombine(folderPidl, child);
                        wchar_t buf[MAX_PATH];
                        if (full && SHGetPathFromIDListW(full, buf) &&
                            (IsSupportedImage(buf) || IsPlayableVideo(buf)))
                            out.push_back(buf);
                        if (full) ILFree(full);
                        CoTaskMemFree(child);
                    }
                }
            }
        }
        if (folderPidl) CoTaskMemFree(folderPidl);
        if (pf) pf->Release();
        if (fv) fv->Release();
        if (sv) sv->Release();
        if (sb) sb->Release();
        if (sp) sp->Release();
        disp->Release();
    }
    sw->Release();
    return out;
}

std::vector<std::wstring> EnumerateDirNaturalOrder(const std::wstring& dir) {
    std::vector<std::wstring> out;
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    std::wstring base = pattern;
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // Match the Explorer-order path (and Explorer defaults): don't surface
        // hidden/system files, or the two enumerators disagree on the item set.
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            continue;
        std::wstring full = base + fd.cFileName;
        if (IsSupportedImage(full) || IsPlayableVideo(full)) out.push_back(std::move(full));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
    });
    return out;
}

DWORD WINAPI ListThreadProc(void* param) {
    std::unique_ptr<ListReq> req(static_cast<ListReq*>(param));
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    const std::wstring dir = req->isFolder ? req->target : ParentDir(req->target);
    std::vector<std::wstring> files = ExplorerViewOrder(dir);
    if (files.empty()) files = EnumerateDirNaturalOrder(dir);

    int start = 0;
    if (!req->isFolder) {
        auto it = std::find_if(files.begin(), files.end(), [&](const std::wstring& f) {
            return lstrcmpiW(f.c_str(), req->target.c_str()) == 0;
        });
        if (it != files.end()) {
            start = (int)(it - files.begin());
        } else {
            files.push_back(req->target);
            start = (int)files.size() - 1;
        }
    }
    auto* done = new ListDone{req->gen, std::move(files), start};
    if (!PostMessageW(req->hwnd, WM_APP_LIST, 0, reinterpret_cast<LPARAM>(done)))
        delete done;
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 0;
}

// ------------------------------------------------------------------- app state

struct CacheEntry {
    std::wstring key;
    std::shared_ptr<DecodedImage> img;
};

struct Layout {
    bool valid = false;
    RECT imgArea{}, stripRc{};
    double scale = 1.0;
    double srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    RECT dest{};
};

struct UiRects {
    bool barVisible = false, saveVisible = false, pageVisible = false;
    bool videoVisible = false;
    RECT rotCcw{}, rotCw{}, save{}, bar{};
    RECT pagePrev{}, pageNext{}, pageBar{};
    RECT videoBar{}, playBtn{}, seekBar{}; // video transport, above the filmstrip
};

struct App {
    HWND hwnd = nullptr;
    HACCEL accel = nullptr;
    int dpi = 96;
    HFONT fontUi = nullptr, fontSym = nullptr, fontBig = nullptr;
    int fontDpi = 0;

    std::vector<std::wstring> files;
    int cur = -1;
    UINT page = 0;
    unsigned listGen = 0;
    bool listPending = false;

    std::shared_ptr<DecodedImage> disp; // current decoded image (unrotated)
    int rot = 0;                        // view rotation, quarter turns CW
    HBITMAP displayBmp = nullptr;       // rot applied + composited on bg
    int dispW = 0, dispH = 0;
    bool decodePending = false;
    bool decodeFailed = false;
    FILETIME lastWrite{};
    bool savePending = false;

    bool fitMode = true;
    double zoom = 1.0;
    double panX = 0, panY = 0;
    bool interactive = false; // fast scaling during wheel/drag
    bool dragging = false;
    POINT dragStart{};
    double dragPanX = 0, dragPanY = 0;

    bool showDetails = false;
    bool showFilmstrip = true;

    bool slideshow = false;          // F5 auto-advance; Esc/F5 exits
    UINT slideshowMs = kSlideshowMs; // per-image dwell; videos play to their end
    bool slideshowShuffle = false;   // advance to a random other item instead

    bool gridMode = false; // full-window thumbnail grid
    int gridCols = 8;      // 16..1, halves/doubles on zoom
    int gridScroll = 0;    // vertical scroll, pixels
    int gridSel = 0;       // keyboard selection in the grid

    // Video playback. player == nullptr (stub build, or D3D11 init failed)
    // means video files are simply not supported.
    HWND videoWnd = nullptr;       // child HWND the engine renders into
    Player* player = nullptr;
    unsigned playerGen = 0;        // open generation; drops stale engine events
    bool videoMode = false;        // current item plays in the engine
    bool videoOpened = false;      // OPENED arrived; child window may show
    bool videoEnded = false;       // playback reached end of media
    bool videoProbed = false;      // videoInfo holds data for the current item
    PlayerMediaInfo videoInfo{};   // filled by the worker thread via WM_APP_PROBED
    std::wstring videoError;       // engine diagnostic captured on PLAYER_EVT_ERROR

    std::wstring failReason;       // specific decode-failure text ("" = generic)
    bool listWasEmpty = false;     // a folder scan finished with zero usable files

    // Optimistic seek: paint the bar at the requested position briefly so a
    // click doesn't rubber-band while the async seek completes.
    double seekFlashPos = -1;      // <0 = none
    DWORD seekFlashTick = 0;

    // Details rows are rebuilt only when something they show changes; building
    // them stats the file on disk, far too heavy for every WM_PAINT.
    std::vector<std::pair<std::wstring, std::wstring>> detailRows;
    bool detailsDirty = true;

    std::vector<CacheEntry> cache; // tiny LRU of decoded images
    DecodeWorker worker;
    Filmstrip strip;
};
App g;

// A video we can actually play: the engine exists (real build, D3D11 up)
// and the extension is a known container. Folder scans, drops and the open
// dialog all gate on this, so builds without the engine (player_stub.cpp)
// behave exactly like the pre-video viewer.
bool IsPlayableVideo(const std::wstring& path) {
    return g.player != nullptr && IsVideoFile(path);
}

// Engine events fire on engine threads — bounce to the UI thread. The user
// pointer carries the open generation so stale events are dropped.
void OnPlayerEvent(void* user, PlayerEvent evt) {
    PostMessageW(g.hwnd, WM_APP_PLAYER, static_cast<WPARAM>(evt),
                 reinterpret_cast<LPARAM>(user));
}

std::wstring CacheKey(const std::wstring& path, UINT page) {
    return path + L"|" + std::to_wstring(page);
}

std::shared_ptr<DecodedImage> CacheGet(const std::wstring& key) {
    for (size_t i = 0; i < g.cache.size(); ++i) {
        if (g.cache[i].key == key) {
            CacheEntry e = g.cache[i];
            g.cache.erase(g.cache.begin() + i);
            g.cache.push_back(e);
            return e.img;
        }
    }
    return nullptr;
}

void CachePut(const std::wstring& key, std::shared_ptr<DecodedImage> img) {
    for (size_t i = 0; i < g.cache.size(); ++i) {
        if (g.cache[i].key == key) {
            g.cache.erase(g.cache.begin() + i);
            break;
        }
    }
    g.cache.push_back({key, std::move(img)});
    while (g.cache.size() > 4) g.cache.erase(g.cache.begin());
}

void CacheRemovePath(const std::wstring& path) {
    const std::wstring prefix = path + L"|";
    g.cache.erase(std::remove_if(g.cache.begin(), g.cache.end(),
                                 [&](const CacheEntry& e) {
                                     return e.key.compare(0, prefix.size(), prefix) == 0;
                                 }),
                  g.cache.end());
}

// ------------------------------------------------------------------ rendering

std::vector<BYTE> RotateBgra(const std::vector<BYTE>& src, UINT w, UINT h, int q,
                             UINT& outW, UINT& outH) {
    outW = (q % 2) ? h : w;
    outH = (q % 2) ? w : h;
    std::vector<BYTE> dst(src.size());
    const UINT32* s = reinterpret_cast<const UINT32*>(src.data());
    UINT32* d = reinterpret_cast<UINT32*>(dst.data());
    for (UINT ny = 0; ny < outH; ++ny) {
        for (UINT nx = 0; nx < outW; ++nx) {
            UINT ox, oy;
            switch (q) {
                case 1:  ox = ny;            oy = h - 1 - nx;  break; // 90 CW
                case 2:  ox = w - 1 - nx;    oy = h - 1 - ny;  break;
                default: ox = w - 1 - ny;    oy = nx;          break; // 270 CW
            }
            d[size_t(ny) * outW + nx] = s[size_t(oy) * w + ox];
        }
    }
    return dst;
}

void RebuildDisplayBitmap() {
    if (g.displayBmp) {
        DeleteObject(g.displayBmp);
        g.displayBmp = nullptr;
    }
    g.dispW = g.dispH = 0;
    if (!g.disp || g.disp->bgra.empty()) return;

    UINT w = g.disp->width, h = g.disp->height;
    const std::vector<BYTE>* pixels = &g.disp->bgra;
    std::vector<BYTE> rotated;
    if (g.rot != 0) {
        rotated = RotateBgra(g.disp->bgra, w, h, g.rot, w, h);
        pixels = &rotated;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    g.displayBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!g.displayBmp) return;

    // Composite straight alpha over the background color once, so every
    // subsequent blit is opaque and cheap.
    const BYTE br = GetRValue(kBg), bgc = GetGValue(kBg), bb = GetBValue(kBg);
    const BYTE* s = pixels->data();
    BYTE* d = static_cast<BYTE*>(bits);
    size_t count = size_t(w) * h;
    for (size_t i = 0; i < count; ++i, s += 4, d += 4) {
        BYTE a = s[3];
        if (a == 255) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        } else {
            d[0] = (BYTE)((s[0] * a + bb * (255 - a)) / 255);
            d[1] = (BYTE)((s[1] * a + bgc * (255 - a)) / 255);
            d[2] = (BYTE)((s[2] * a + br * (255 - a)) / 255);
        }
        d[3] = 255;
    }
    g.dispW = (int)w;
    g.dispH = (int)h;
}

Layout ComputeLayout(const RECT& client) {
    Layout L;
    int stripH = g.showFilmstrip ? g.strip.Height(g.dpi) : 0;
    L.imgArea = {0, 0, client.right, client.bottom - stripH};
    L.stripRc = {0, L.imgArea.bottom, client.right, client.bottom};
    if (!g.displayBmp || g.dispW <= 0 || g.dispH <= 0) return L;

    const double areaW = (std::max)(1L, L.imgArea.right - L.imgArea.left);
    const double areaH = (std::max)(1L, L.imgArea.bottom - L.imgArea.top);
    const double fit = (std::min)(1.0, (std::min)(areaW / g.dispW, areaH / g.dispH));
    L.scale = g.fitMode ? fit : g.zoom;

    const double viewW = g.dispW * L.scale, viewH = g.dispH * L.scale;
    if (viewW <= areaW) {
        L.srcX = 0;
        L.srcW = g.dispW;
        // Round the offset once so the left/right margins match; truncating
        // offset and width independently can leave them a pixel apart.
        L.dest.left = L.imgArea.left + (LONG)((areaW - viewW) / 2 + 0.5);
        L.dest.right = L.dest.left + (LONG)(viewW + 0.5);
        g.panX = 0;
    } else {
        const double maxPan = g.dispW - areaW / L.scale;
        g.panX = (std::max)(0.0, (std::min)(g.panX, maxPan));
        L.srcX = g.panX;
        L.srcW = areaW / L.scale;
        L.dest.left = L.imgArea.left;
        L.dest.right = L.imgArea.right;
    }
    if (viewH <= areaH) {
        L.srcY = 0;
        L.srcH = g.dispH;
        L.dest.top = L.imgArea.top + (LONG)((areaH - viewH) / 2 + 0.5);
        L.dest.bottom = L.dest.top + (LONG)(viewH + 0.5);
        g.panY = 0;
    } else {
        const double maxPan = g.dispH - areaH / L.scale;
        g.panY = (std::max)(0.0, (std::min)(g.panY, maxPan));
        L.srcY = g.panY;
        L.srcH = areaH / L.scale;
        L.dest.top = L.imgArea.top;
        L.dest.bottom = L.imgArea.bottom;
    }
    L.valid = true;
    return L;
}

int Scaled(int v) { return MulDiv(v, g.dpi, 96); }

// ------------------------------------------------------------------ grid view

RECT ClientRect(); // fwd decls (defined below)
void DrawCenteredText(HDC dc, const RECT& rc, const wchar_t* text);
void PositionVideoWindow();
void StopSlideshow();

struct GridLayout {
    int cols = 1, cell = 1, rows = 0, contentH = 0;
    RECT area{};
};

GridLayout ComputeGrid(const RECT& client) {
    GridLayout gl;
    gl.area = client;
    gl.cols = (std::max)(1, g.gridCols);
    gl.cell = (std::max)(1L, client.right - client.left) / gl.cols;
    if (gl.cell < 1) gl.cell = 1;
    // Integer division leaves up to cols-1 spare pixels; split them so the
    // grid is centered instead of hugging the left edge with a dead gutter.
    const int slack = (client.right - client.left) - gl.cols * gl.cell;
    if (slack > 0) {
        gl.area.left += slack / 2;
        gl.area.right = gl.area.left + gl.cols * gl.cell;
    }
    const int n = (int)g.files.size();
    gl.rows = (n + gl.cols - 1) / gl.cols;
    gl.contentH = gl.rows * gl.cell;
    const int viewH = client.bottom - client.top;
    g.gridScroll = (std::max)(0, (std::min)(g.gridScroll, gl.contentH - viewH));
    return gl;
}

RECT GridCellRect(const GridLayout& gl, int index) {
    const int row = index / gl.cols, col = index % gl.cols;
    RECT rc;
    rc.left = gl.area.left + col * gl.cell;
    rc.top = gl.area.top + row * gl.cell - g.gridScroll;
    rc.right = rc.left + gl.cell;
    rc.bottom = rc.top + gl.cell;
    return rc;
}

void GridEnsureVisible() {
    RECT client = ClientRect();
    GridLayout gl = ComputeGrid(client);
    if (g.files.empty()) return;
    g.gridSel = (std::max)(0, (std::min)(g.gridSel, (int)g.files.size() - 1));
    const int rowTop = (g.gridSel / gl.cols) * gl.cell;
    const int viewH = client.bottom - client.top;
    if (rowTop < g.gridScroll) g.gridScroll = rowTop;
    else if (rowTop + gl.cell > g.gridScroll + viewH)
        g.gridScroll = rowTop + gl.cell - viewH;
    if (g.gridScroll < 0) g.gridScroll = 0;
}

void ToggleGrid() {
    g.gridMode = !g.gridMode;
    if (g.gridMode) {
        StopSlideshow(); // the grid pauses video below; a slideshow ends instead
        g.gridSel = (std::max)(0, g.cur);
        GridEnsureVisible();
        // The hidden video child shouldn't keep playing audio under the
        // grid; no auto-resume on leaving — the user resumes manually.
        if (g.videoMode && g.player && !player_is_paused(g.player))
            player_toggle_pause(g.player);
    }
    PositionVideoWindow(); // grid paints the whole client; hide the video child
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void GridMoveSel(int delta) {
    if (g.files.empty()) return;
    const int n = (int)g.files.size();
    const int cols = (std::max)(1, g.gridCols);
    int sel = g.gridSel + delta;
    // Vertical moves (±cols) keep the column: going up off the top row is a
    // no-op (not a jump to item 0), and going down is a no-op unless it lands
    // in a partial last row, where it clamps to the final item like Explorer.
    if (delta == cols || delta == -cols) {
        if (sel < 0) return;                  // above the first row
        if (sel >= n) {
            if (g.gridSel / cols == (n - 1) / cols) return; // already last row
            sel = n - 1;                      // into a partial last row
        }
    } else {
        sel = (std::max)(0, (std::min)(sel, n - 1));
    }
    if (sel == g.gridSel) return;
    g.gridSel = sel;
    GridEnsureVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Zoom in = bigger cells = fewer columns (16 -> 8 -> 4 -> 2 -> 1).
void GridZoom(bool in) {
    int cols = g.gridCols;
    cols = in ? cols / 2 : cols * 2;
    cols = (std::max)(1, (std::min)(cols, 16));
    if (cols == g.gridCols) return;
    g.gridCols = cols;
    GridEnsureVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void NavigateTo(int index, bool resetPage); // fwd decl (defined below)

void OpenFromGrid(int index) {
    if (index < 0 || index >= (int)g.files.size()) return;
    g.gridMode = false;
    NavigateTo(index, true);
}

int GridHitTest(POINT pt, const GridLayout& gl) {
    if (pt.x < gl.area.left || pt.x >= gl.area.right) return -1;
    const int col = (pt.x - gl.area.left) / gl.cell;
    if (col >= gl.cols) return -1;
    const int row = (pt.y - gl.area.top + g.gridScroll) / gl.cell;
    if (row < 0) return -1;
    const int idx = row * gl.cols + col;
    return (idx < (int)g.files.size()) ? idx : -1;
}

// Small shell/type icon for a file, cached by lowercase extension. Looked up by
// attribute (SHGFI_USEFILEATTRIBUTES) so it never touches disk and one icon
// serves every file of a type. Drawn as a bottom-right badge over thumbnails so
// the file kind stays visible even when the content thumbnail looks generic.
HICON TypeIconFor(const std::wstring& path) {
    static std::unordered_map<std::wstring, HICON> cache; // by lowercase extension
    std::wstring ext;
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) ext = path.substr(dot);
    if (!ext.empty()) CharLowerBuffW(&ext[0], (DWORD)ext.size());
    auto it = cache.find(ext);
    if (it != cache.end()) return it->second;
    SHFILEINFOW sfi{};
    HICON icon = nullptr;
    if (SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES))
        icon = sfi.hIcon; // bounded by distinct-extension count (not session
                          // length); the OS reclaims these handles at exit
    cache[ext] = icon;
    return icon;
}

// Overlay the file-type icon in the bottom-right of a thumbnail, sized to ~15%
// of its width (clamped so it stays legible on tiny cells and modest on large).
void DrawTypeBadge(HDC dc, const std::wstring& path, int tx, int ty, int tw, int th) {
    HICON icon = TypeIconFor(path);
    if (!icon) return;
    int bw = (std::max)(Scaled(12), (int)(tw * 0.15 + 0.5));
    bw = (std::min)(bw, (std::min)(tw, th));
    if (bw <= 0) return;
    int margin = (std::max)(1, bw / 8);
    int bx = tx + tw - bw - margin;
    int by = ty + th - bw - margin;
    DrawIconEx(dc, bx, by, icon, bw, bw, 0, nullptr, DI_NORMAL);
}

void PaintGrid(HDC dc, const RECT& client) {
    GridLayout gl = ComputeGrid(client);
    if (g.files.empty()) {
        DrawCenteredText(dc, client,
                         g.listPending ? L"Loading…"
                         : g.listWasEmpty
                             ? L"No supported images or videos in this folder."
                             : L"Drop images or a folder here — or press Ctrl+O");
        return;
    }
    const int viewH = client.bottom - client.top;
    const int firstRow = g.gridScroll / gl.cell;
    const int lastRow = (std::min)(gl.rows - 1, (g.gridScroll + viewH) / gl.cell);
    const int pad = (std::max)(2, gl.cell / 32);
    const int thumbClass = Filmstrip::SizeClassFor(gl.cell);

    HDC mem = CreateCompatibleDC(dc);
    for (int row = firstRow; row <= lastRow; ++row) {
        for (int col = 0; col < gl.cols; ++col) {
            const int i = row * gl.cols + col;
            if (i >= (int)g.files.size()) break;
            const std::wstring& path = g.files[i];
            RECT cellRc = GridCellRect(gl, i);
            InflateRect(&cellRc, -pad, -pad);

            if (!g.strip.GetThumb(path, thumbClass))
                g.strip.Request(path, thumbClass);
            HBITMAP thumb = g.strip.GetThumbAny(path);
            const int cw = cellRc.right - cellRc.left;
            const int chh = cellRc.bottom - cellRc.top;
            int tx = cellRc.left, ty = cellRc.top, tw = cw, th = chh;
            if (thumb) {
                BITMAP bm{};
                GetObjectW(thumb, sizeof(bm), &bm);
                double s = (std::min)((double)cw / bm.bmWidth, (double)chh / bm.bmHeight);
                tw = (std::max)(1, (int)(bm.bmWidth * s));
                th = (std::max)(1, (int)(bm.bmHeight * s));
                tx = cellRc.left + (cw - tw) / 2;
                ty = cellRc.top + (chh - th) / 2;
                HGDIOBJ old = SelectObject(mem, thumb);
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                StretchBlt(dc, tx, ty, tw, th, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                SelectObject(mem, old);
            } else {
                HBRUSH ph = CreateSolidBrush(RGB(46, 46, 46));
                FillRect(dc, &cellRc, ph);
                DeleteObject(ph);
            }
            // File-type badge over the thumbnail's bottom-right corner.
            DrawTypeBadge(dc, path, tx, ty, tw, th);

            if (i == g.gridSel) {
                HBRUSH hl = CreateSolidBrush(kAccent);
                RECT b = cellRc;
                InflateRect(&b, pad / 2 + 2, pad / 2 + 2);
                FrameRect(dc, &b, hl);
                InflateRect(&b, -1, -1);
                FrameRect(dc, &b, hl);
                DeleteObject(hl);
            }
        }
    }
    DeleteDC(mem);
}

UiRects ComputeUiRects(const RECT& imgArea) {
    UiRects r;
    if (g.cur < 0) return r;
    const int btnW = Scaled(44), btnH = Scaled(30), gap = Scaled(6), pad = Scaled(8);
    const bool canSave = g.rot != 0 && g.disp && !g.savePending &&
                         FindDecoder(g.files[g.cur]) &&
                         FindDecoder(g.files[g.cur])->CanSaveRotation(g.files[g.cur], *g.disp);
    r.saveVisible = canSave;
    const int saveW = canSave ? Scaled(64) + gap : 0;
    const int barW = 2 * btnW + gap + saveW + 2 * pad;
    const int barH = btnH + 2 * pad;
    const int cx = (imgArea.left + imgArea.right) / 2;
    const int by = imgArea.bottom - Scaled(16) - barH;
    r.bar = {cx - barW / 2, by, cx + barW / 2, by + barH};
    r.rotCcw = {r.bar.left + pad, by + pad, r.bar.left + pad + btnW, by + pad + btnH};
    r.rotCw = {r.rotCcw.right + gap, r.rotCcw.top, r.rotCcw.right + gap + btnW, r.rotCcw.bottom};
    if (canSave)
        r.save = {r.rotCw.right + gap, r.rotCw.top, r.rotCw.right + gap + Scaled(64), r.rotCw.bottom};
    r.barVisible = g.disp != nullptr;

    if (g.disp && g.disp->pageCount > 1) {
        r.pageVisible = true;
        const int pw = Scaled(180), ph = Scaled(30);
        r.pageBar = {cx - pw / 2, imgArea.top + Scaled(12), cx + pw / 2,
                     imgArea.top + Scaled(12) + ph};
        r.pagePrev = {r.pageBar.left, r.pageBar.top, r.pageBar.left + Scaled(36), r.pageBar.bottom};
        r.pageNext = {r.pageBar.right - Scaled(36), r.pageBar.top, r.pageBar.right, r.pageBar.bottom};
    }

    if (g.videoMode) {
        // Transport strip along the bottom of imgArea; the video child is
        // sized to stop above it (PositionVideoWindow), so the parent can
        // draw and hit-test here.
        r.videoVisible = true;
        const int m = Scaled(8);
        r.videoBar = {imgArea.left, imgArea.bottom - Scaled(kVideoBarH),
                      imgArea.right, imgArea.bottom};
        r.playBtn = {r.videoBar.left + m, r.videoBar.top + Scaled(4),
                     r.videoBar.left + m + Scaled(56), r.videoBar.bottom - Scaled(4)};
        const int mid = (r.videoBar.top + r.videoBar.bottom) / 2;
        r.seekBar = {r.playBtn.right + m + Scaled(96), mid - Scaled(3),
                     r.videoBar.right - m, mid + Scaled(3)};
        // A very narrow window can push left past right; collapse to empty
        // rather than an inverted rect that breaks the fill math and hit test.
        if (r.seekBar.left > r.seekBar.right) r.seekBar.left = r.seekBar.right;
    }
    return r;
}

void EnsureFonts() {
    if (g.fontDpi == g.dpi && g.fontUi) return;
    if (g.fontUi) DeleteObject(g.fontUi);
    if (g.fontSym) DeleteObject(g.fontSym);
    if (g.fontBig) DeleteObject(g.fontBig);
    g.fontUi = CreateFontW(-MulDiv(10, g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fontSym = CreateFontW(-MulDiv(12, g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Symbol");
    g.fontBig = CreateFontW(-MulDiv(12, g.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fontDpi = g.dpi;
}

void DrawButton(HDC dc, const RECT& rc, const wchar_t* label, HFONT font) {
    HBRUSH b = CreateSolidBrush(RGB(45, 45, 45));
    FillRect(dc, &rc, b);
    DeleteObject(b);
    HBRUSH fr = CreateSolidBrush(kPanelBorder);
    FrameRect(dc, &rc, fr);
    DeleteObject(fr);
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, kText);
    SetBkMode(dc, TRANSPARENT);
    RECT t = rc;
    DrawTextW(dc, label, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

void DrawCenteredText(HDC dc, const RECT& rc, const wchar_t* text) {
    EnsureFonts();
    HGDIOBJ oldFont = SelectObject(dc, g.fontBig);
    SetTextColor(dc, kTextDim);
    SetBkMode(dc, TRANSPARENT);
    RECT t = rc;
    DrawTextW(dc, text, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}

// Multi-line variant for messages that carry a path or a diagnostic on
// separate lines (DT_SINGLELINE would render the newlines as glyphs).
void DrawCenteredTextML(HDC dc, const RECT& rc, const wchar_t* text) {
    EnsureFonts();
    HGDIOBJ oldFont = SelectObject(dc, g.fontBig);
    SetTextColor(dc, kTextDim);
    SetBkMode(dc, TRANSPARENT);
    RECT m = rc;
    DrawTextW(dc, text, -1, &m, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    RECT t = rc;
    t.top = (std::max)(rc.top, rc.top + ((rc.bottom - rc.top) - (m.bottom - m.top)) / 2);
    t.bottom = rc.bottom;
    DrawTextW(dc, text, -1, &t, DT_CENTER | DT_WORDBREAK);
    SelectObject(dc, oldFont);
}

std::vector<std::pair<std::wstring, std::wstring>> BuildDetails() {
    std::vector<std::pair<std::wstring, std::wstring>> rows;
    if (g.cur < 0) return rows;
    const std::wstring& path = g.files[g.cur];
    rows.emplace_back(L"Name", PathFindFileNameW(path.c_str()));
    rows.emplace_back(L"Folder", ParentDir(path));

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER sz{};
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = fad.nFileSizeHigh;
        wchar_t buf[64];
        StrFormatByteSizeW((LONGLONG)sz.QuadPart, buf, 64);
        rows.emplace_back(L"Size", buf);
        auto fmtTime = [](const FILETIME& ft) -> std::wstring {
            FILETIME lft;
            SYSTEMTIME st;
            FileTimeToLocalFileTime(&ft, &lft);
            FileTimeToSystemTime(&lft, &st);
            wchar_t d[64], t[64];
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, d, 64, nullptr);
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, t, 64);
            return std::wstring(d) + L" " + t;
        };
        rows.emplace_back(L"Created", fmtTime(fad.ftCreationTime));
        rows.emplace_back(L"Modified", fmtTime(fad.ftLastWriteTime));
    }
    if (g.disp) {
        wchar_t buf[64];
        swprintf(buf, 64, L"%u x %u", g.disp->width, g.disp->height);
        rows.emplace_back(L"Dimensions", buf);
        if (g.disp->pageCount > 1) {
            swprintf(buf, 64, L"%u / %u", g.page + 1, g.disp->pageCount);
            rows.emplace_back(L"Page", buf);
        }
        for (const auto& m : g.disp->meta) rows.push_back(m);
    } else if (g.videoMode && g.videoProbed) {
        // Filled asynchronously by the worker's player_probe (WM_APP_PROBED);
        // never query the engine here — Paint calls this every frame.
        wchar_t buf[64];
        if (g.videoInfo.width > 0) {
            swprintf(buf, 64, L"%d x %d", g.videoInfo.width, g.videoInfo.height);
            rows.emplace_back(L"Dimensions", buf);
        }
        if (g.videoInfo.duration_sec > 0) {
            const int s = (int)g.videoInfo.duration_sec;
            swprintf(buf, 64, L"%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
            rows.emplace_back(L"Duration", buf);
        }
        if (g.videoInfo.video_codec[0]) rows.emplace_back(L"Video", g.videoInfo.video_codec);
        if (g.videoInfo.audio_codec[0]) {
            std::wstring a = g.videoInfo.audio_codec;
            if (g.videoInfo.audio_tracks > 1)
                a += L" (" + std::to_wstring(g.videoInfo.audio_tracks) + L" tracks)";
            rows.emplace_back(L"Audio", a);
        }
    }
    return rows;
}

// Cached details rows. BuildDetails stats the file and formats dates — much
// too heavy to run on every WM_PAINT (the video timer repaints continuously).
const std::vector<std::pair<std::wstring, std::wstring>>& Details() {
    if (g.detailsDirty) {
        g.detailRows = BuildDetails();
        g.detailsDirty = false;
    }
    return g.detailRows;
}

// Details panel rectangle in client coordinates — shared by Paint (which
// draws it) and PositionVideoWindow (which punches it out of the video
// child's window region so the drawing shows through).
RECT DetailsPanelRect(const RECT& imgArea, size_t rows) {
    const int pad = Scaled(12);
    RECT panel = {imgArea.left + Scaled(12), imgArea.top + Scaled(12), 0, 0};
    panel.right = panel.left + Scaled(380);
    panel.bottom = panel.top + 2 * pad + (int)rows * Scaled(20);
    return panel;
}

void Paint(HDC dcWin, const RECT& client) {
    EnsureFonts();
    const int cw = client.right, ch = client.bottom;
    if (cw <= 0 || ch <= 0) return;
    HDC dc = CreateCompatibleDC(dcWin);
    HBITMAP back = CreateCompatibleBitmap(dcWin, cw, ch);
    HGDIOBJ oldBack = SelectObject(dc, back);

    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &client, bg);
    DeleteObject(bg);

    if (g.gridMode) {
        PaintGrid(dc, client);
        BitBlt(dcWin, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBack);
        DeleteObject(back);
        DeleteDC(dc);
        return;
    }

    Layout L = ComputeLayout(client);
    if (L.valid) {
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g.displayBmp);
        SetStretchBltMode(dc, g.interactive ? COLORONCOLOR : HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, L.dest.left, L.dest.top, L.dest.right - L.dest.left,
                   L.dest.bottom - L.dest.top, mem, (int)L.srcX, (int)L.srcY,
                   (int)L.srcW, (int)L.srcH, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
    } else if (g.cur >= 0 && g.decodePending) {
        // Blurry-then-sharp: show the filmstrip thumbnail while decoding.
        HBITMAP thumb = g.strip.GetThumbAny(g.files[g.cur]);
        if (thumb) {
            BITMAP bm{};
            GetObjectW(thumb, sizeof(bm), &bm);
            double areaW = (std::max)(1L, L.imgArea.right - L.imgArea.left);
            double areaH = (std::max)(1L, L.imgArea.bottom - L.imgArea.top);
            double s = (std::min)(areaW / bm.bmWidth, areaH / bm.bmHeight);
            int tw = (std::max)(1, (int)(bm.bmWidth * s));
            int th = (std::max)(1, (int)(bm.bmHeight * s));
            int tx = L.imgArea.left + ((int)areaW - tw) / 2;
            int ty = L.imgArea.top + ((int)areaH - th) / 2;
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, thumb);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, tx, ty, tw, th, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        } else {
            DrawCenteredText(dc, L.imgArea, L"Loading…");
        }
    } else if (g.cur >= 0 && g.decodeFailed) {
        // Prefer the specific diagnosis (engine error / empty file / corrupt)
        // over the generic line, so distinct failures look distinct.
        DrawCenteredTextML(dc, L.imgArea,
                           !g.videoError.empty()  ? g.videoError.c_str()
                           : !g.failReason.empty() ? g.failReason.c_str()
                                                   : L"Can't display this file.");
    } else if (g.videoMode && !g.videoOpened) {
        // player_open is async; without this the user stares at a bare black
        // child window until PLAYER_EVT_OPENED arrives.
        DrawCenteredText(dc, L.imgArea, L"Loading…");
    } else if (g.cur < 0) {
        DrawCenteredText(dc, L.imgArea,
                         g.listPending ? L"Loading…"
                         : g.listWasEmpty
                             ? L"No supported images or videos in this folder."
                             : L"Drop images or a folder here — or press Ctrl+O");
    }

    UiRects ui = ComputeUiRects(L.imgArea);
    if (ui.barVisible) {
        HBRUSH pb = CreateSolidBrush(kPanelBg);
        FillRect(dc, &ui.bar, pb);
        DeleteObject(pb);
        HBRUSH fr = CreateSolidBrush(kPanelBorder);
        FrameRect(dc, &ui.bar, fr);
        DeleteObject(fr);
        DrawButton(dc, ui.rotCcw, L"↺", g.fontSym);
        DrawButton(dc, ui.rotCw, L"↻", g.fontSym);
        if (ui.saveVisible) DrawButton(dc, ui.save, L"Save", g.fontUi);
    }
    if (ui.pageVisible) {
        HBRUSH pb = CreateSolidBrush(kPanelBg);
        FillRect(dc, &ui.pageBar, pb);
        DeleteObject(pb);
        HBRUSH fr = CreateSolidBrush(kPanelBorder);
        FrameRect(dc, &ui.pageBar, fr);
        DeleteObject(fr);
        DrawButton(dc, ui.pagePrev, L"◀", g.fontSym);
        DrawButton(dc, ui.pageNext, L"▶", g.fontSym);
        wchar_t buf[32];
        swprintf(buf, 32, L"%u / %u", g.page + 1, g.disp ? g.disp->pageCount : 1);
        RECT mid = {ui.pagePrev.right, ui.pageBar.top, ui.pageNext.left, ui.pageBar.bottom};
        HGDIOBJ oldFont = SelectObject(dc, g.fontUi);
        SetTextColor(dc, kText);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, buf, -1, &mid, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
    }

    if (ui.videoVisible) {
        HBRUSH pb = CreateSolidBrush(kPanelBg);
        FillRect(dc, &ui.videoBar, pb);
        DeleteObject(pb);
        const bool showPlay = !g.player || player_is_paused(g.player) || g.videoEnded;
        DrawButton(dc, ui.playBtn, showPlay ? L"Play" : L"Pause", g.fontUi);

        const double dur = g.player ? player_duration(g.player) : 0.0;
        double pos = g.videoEnded ? dur : (g.player ? player_position(g.player) : 0.0);
        // A just-requested seek paints at its target briefly, so the fill
        // doesn't rubber-band back to the pre-seek position while the async
        // seek completes.
        if (g.seekFlashPos >= 0) {
            if (GetTickCount() - g.seekFlashTick < 400) pos = g.seekFlashPos;
            else g.seekFlashPos = -1;
        }
        // H:MM:SS once an hour is involved (both sides share the format so
        // the readout doesn't flip widths); plain M:SS below that.
        const bool hours = dur >= 3600 || pos >= 3600;
        auto fmtTime = [hours](wchar_t* out, size_t n, double s) {
            int v = (int)s;
            if (hours) swprintf(out, n, L"%d:%02d:%02d", v / 3600, (v / 60) % 60, v % 60);
            else swprintf(out, n, L"%d:%02d", v / 60, v % 60);
        };
        wchar_t ps[24], ds[24], buf[64];
        fmtTime(ps, 24, pos);
        fmtTime(ds, 24, dur);
        swprintf(buf, 64, L"%ls / %ls", ps, ds);
        RECT trc = {ui.playBtn.right + Scaled(8), ui.videoBar.top,
                    ui.seekBar.left - Scaled(4), ui.videoBar.bottom};
        HGDIOBJ oldFont = SelectObject(dc, g.fontUi);
        SetTextColor(dc, kTextDim);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, buf, -1, &trc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);

        HBRUSH track = CreateSolidBrush(RGB(60, 60, 60));
        FillRect(dc, &ui.seekBar, track);
        DeleteObject(track);
        if (dur > 0) {
            const double f = (std::max)(0.0, (std::min)(1.0, pos / dur));
            RECT fill = ui.seekBar;
            fill.right = fill.left + (LONG)((ui.seekBar.right - ui.seekBar.left) * f);
            HBRUSH fb = CreateSolidBrush(kAccent);
            FillRect(dc, &fill, fb);
            DeleteObject(fb);
        }
    }

    if (g.showDetails && g.cur >= 0) {
        const auto& rows = Details();
        const int pad = Scaled(12), lineH = Scaled(20), labelW = Scaled(100);
        RECT panel = DetailsPanelRect(L.imgArea, rows.size());
        HBRUSH pb = CreateSolidBrush(kPanelBg);
        FillRect(dc, &panel, pb);
        DeleteObject(pb);
        HBRUSH fr = CreateSolidBrush(kPanelBorder);
        FrameRect(dc, &panel, fr);
        DeleteObject(fr);
        HGDIOBJ oldFont = SelectObject(dc, g.fontUi);
        SetBkMode(dc, TRANSPARENT);
        int y = panel.top + pad;
        for (const auto& row : rows) {
            SetTextColor(dc, kTextDim);
            RECT lr = {panel.left + pad, y, panel.left + pad + labelW, y + lineH};
            DrawTextW(dc, row.first.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SetTextColor(dc, kText);
            RECT vr = {lr.right + Scaled(6), y, panel.right - pad, y + lineH};
            DrawTextW(dc, row.second.c_str(), -1, &vr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_PATH_ELLIPSIS);
            y += lineH;
        }
        SelectObject(dc, oldFont);
    }

    if (g.showFilmstrip) g.strip.Draw(dc, L.stripRc);

    BitBlt(dcWin, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBack);
    DeleteObject(back);
    DeleteDC(dc);
}

// -------------------------------------------------------------------- actions

RECT ClientRect() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    return rc;
}

void UpdateTitle() {
    std::wstring t;
    if (g.cur >= 0) {
        t = PathFindFileNameW(g.files[g.cur].c_str());
        if (g.rot != 0) t += L"*";
        t += L" (" + std::to_wstring(g.cur + 1) + L"/" + std::to_wstring(g.files.size()) + L")";
        if (g.slideshow) t += L" — Slideshow (Esc to exit)";
        t += L" — Media Gallery";
    } else {
        t = L"Media Gallery";
    }
    SetWindowTextW(g.hwnd, t.c_str());
}

void StopSlideshow() {
    if (!g.slideshow) return;
    g.slideshow = false;
    KillTimer(g.hwnd, kTimerSlideshow);
    UpdateTitle();
}

// Show/hide the video child and fit it to the image area, leaving the
// transport strip at the bottom for the parent to draw into. When the
// details panel is open, its rectangle is punched out of the child's
// window region so the parent's panel pixels show through
// (WS_CLIPCHILDREN blocks drawing over the child). Call wherever the
// layout inputs change (size, DPI, filmstrip/grid/panel toggles,
// navigation, probe arrival).
void PositionVideoWindow() {
    if (!g.videoWnd) return;
    // Keep the child hidden until OPENED so the "Loading…" hint in the parent
    // shows through instead of a bare black rectangle.
    if (!g.videoMode || g.gridMode || !g.videoOpened) {
        SetWindowRgn(g.videoWnd, nullptr, TRUE);
        ShowWindow(g.videoWnd, SW_HIDE);
        return;
    }
    RECT rc = ClientRect();
    Layout L = ComputeLayout(rc);
    RECT v = L.imgArea;
    v.bottom = (std::max)(v.top + 1, v.bottom - Scaled(kVideoBarH));
    SetWindowPos(g.videoWnd, nullptr, v.left, v.top, v.right - v.left,
                 v.bottom - v.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (g.showDetails && g.cur >= 0) {
        RECT panel = DetailsPanelRect(L.imgArea, Details().size());
        OffsetRect(&panel, -v.left, -v.top); // child-relative coordinates
        HRGN rgn = CreateRectRgn(0, 0, v.right - v.left, v.bottom - v.top);
        HRGN hole = CreateRectRgn(panel.left, panel.top, panel.right, panel.bottom);
        CombineRgn(rgn, rgn, hole, RGN_DIFF);
        DeleteObject(hole);
        SetWindowRgn(g.videoWnd, rgn, TRUE); // the system owns rgn from here
    } else {
        SetWindowRgn(g.videoWnd, nullptr, TRUE);
    }
    if (g.player) player_notify_resize(g.player);
}

// Remember a just-requested seek target so the transport bar paints there
// immediately instead of rubber-banding until the async seek lands.
void SeekFlash(double target) {
    const double dur = g.player ? player_duration(g.player) : 0.0;
    if (dur > 0) target = (std::max)(0.0, (std::min)(target, dur));
    g.seekFlashPos = target;
    g.seekFlashTick = GetTickCount();
}

void VideoPlayPause() {
    if (!g.player || !g.videoMode) return;
    if (g.videoEnded) {
        SeekFlash(0);
        player_seek_to(g.player, 0); // restart from the top after the end
        if (player_is_paused(g.player)) player_toggle_pause(g.player);
        g.videoEnded = false;
    } else {
        player_toggle_pause(g.player);
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void VideoSeekBy(double seconds) {
    if (!g.player || !g.videoMode) return;
    SeekFlash(player_position(g.player) + seconds);
    player_seek_rel(g.player, seconds);
    g.videoEnded = false;
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// The slideshow's step: the next item, or with shuffle on a random *other*
// item — draw from the n-1 non-current slots so it never repeats in place.
void SlideshowAdvance() {
    const int n = (int)g.files.size();
    if (g.slideshowShuffle && n > 1) {
        int idx = rand() % (n - 1);
        if (idx >= g.cur) ++idx;
        NavigateTo(idx, true); // fwd decl has no default arg yet
    } else {
        NavigateTo(g.cur + 1, true);
    }
}

// F5: images advance every g.slideshowMs; a video plays to its end and the
// ENDED event advances instead (LoadCurrent swaps the timer accordingly).
void ToggleSlideshow() {
    if (g.slideshow) {
        StopSlideshow();
        return;
    }
    if (g.files.empty()) return;
    g.slideshow = true;
    if (g.gridMode) {
        OpenFromGrid(g.gridSel); // straight to the viewer; LoadCurrent arms us
    } else if (g.videoMode) {
        // Never through the grid's pause path: a paused/finished video resumes.
        if (g.player && (player_is_paused(g.player) || g.videoEnded)) VideoPlayPause();
    } else {
        SetTimer(g.hwnd, kTimerSlideshow, g.slideshowMs, nullptr);
    }
    UpdateTitle();
}

void PrefetchNeighbors() {
    const int n = (int)g.files.size();
    if (n <= 1 || g.cur < 0) return;
    for (int step : {1, -1}) {
        int idx = ((g.cur + step) % n + n) % n;
        if (idx == g.cur) continue;
        if (IsVideoFile(g.files[idx])) continue; // videos have no decode path
        if (!CacheGet(CacheKey(g.files[idx], 0)))
            g.worker.RequestPrefetch(g.files[idx], 0);
    }
}

void CaptureLastWrite(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        g.lastWrite = fad.ftLastWriteTime;
    else
        g.lastWrite = FILETIME{};
}

void LoadCurrent() {
    g.disp.reset();
    if (g.displayBmp) {
        DeleteObject(g.displayBmp);
        g.displayBmp = nullptr;
    }
    g.dispW = g.dispH = 0;
    g.decodeFailed = false;
    g.decodePending = false;
    g.videoEnded = false;
    g.videoOpened = false;
    g.videoProbed = false;
    g.videoError.clear();
    g.failReason.clear();
    g.seekFlashPos = -1;
    g.detailsDirty = true;

    const bool video = g.cur >= 0 && IsPlayableVideo(g.files[g.cur]);
    if (g.videoMode && !video) {
        if (g.player) player_close(g.player);
        KillTimer(g.hwnd, kTimerVideo);
    }
    g.videoMode = video;

    if (g.cur >= 0) {
        const std::wstring& path = g.files[g.cur];
        CaptureLastWrite(path);
        if (video) {
            // Close first so no stale event can carry the new generation,
            // then open asynchronously; OPENED/ENDED/ERROR arrive as
            // WM_APP_PLAYER. The worker probes metadata off the UI thread.
            player_close(g.player);
            player_set_event_callback(
                g.player, OnPlayerEvent,
                reinterpret_cast<void*>(static_cast<UINT_PTR>(++g.playerGen)));
            player_open(g.player, path.c_str());
            // Landed here while the grid is up (folder drop, delete): the
            // child window stays hidden, so don't let audio play invisibly.
            if (g.gridMode && !player_is_paused(g.player))
                player_toggle_pause(g.player);
            g.worker.RequestProbe(path);
            SetTimer(g.hwnd, kTimerVideo, 100, nullptr); // smooth seek-fill motion
        } else if (auto img = CacheGet(CacheKey(path, g.page))) {
            g.disp = img;
            RebuildDisplayBitmap();
            PrefetchNeighbors();
        } else {
            g.decodePending = true;
            g.worker.RequestCurrent(path, g.page);
        }
        g.strip.SetCurrent(g.cur);
    }
    if (g.slideshow) {
        if (g.cur < 0) StopSlideshow();
        else if (g.videoMode) KillTimer(g.hwnd, kTimerSlideshow); // ENDED advances
        else SetTimer(g.hwnd, kTimerSlideshow, g.slideshowMs, nullptr);
    }
    PositionVideoWindow();
    UpdateTitle();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// Offer to persist an unsaved rotation before leaving the image.
void MaybeCommitRotation() {
    if (g.rot == 0 || !g.disp || g.cur < 0) return;
    // A save for this rotation is already queued (Ctrl+S then navigate):
    // asking again would rotate the file a second time.
    if (g.savePending) {
        g.rot = 0;
        return;
    }
    // By value: MessageBoxW below pumps a modal loop, during which a folder
    // scan's WM_APP_LIST can replace g.files and free the referenced string.
    const std::wstring path = g.files[g.cur];
    ImageDecoder* dec = FindDecoder(path);
    if (dec && dec->CanSaveRotation(path, *g.disp)) {
        // Silence the slideshow while the prompt is up — its timer would fire
        // into the modal loop and re-enter navigation under our feet.
        KillTimer(g.hwnd, kTimerSlideshow);
        std::wstring msg = L"Save the rotation to \"";
        msg += PathFindFileNameW(path.c_str());
        msg += L"\"?";
        if (MessageBoxW(g.hwnd, msg.c_str(), L"Media Gallery",
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            g.savePending = true;
            g.worker.RequestSave(path, g.rot);
        }
        if (g.slideshow && !g.gridMode && !g.videoMode)
            SetTimer(g.hwnd, kTimerSlideshow, g.slideshowMs, nullptr);
    }
    g.rot = 0;
}

void NavigateTo(int index, bool resetPage = true) {
    if (g.files.empty()) return;
    const int n = (int)g.files.size();
    index = ((index % n) + n) % n;
    MaybeCommitRotation();
    g.cur = index;
    if (resetPage) g.page = 0;
    g.rot = 0;
    g.fitMode = true;
    LoadCurrent();
}

void SetPage(int page) {
    if (!g.disp || g.disp->pageCount <= 1) return;
    page = (std::max)(0, (std::min)(page, (int)g.disp->pageCount - 1));
    if ((UINT)page == g.page) return;
    g.page = (UINT)page;
    g.rot = 0;
    g.fitMode = true;
    LoadCurrent();
}

void AdoptFileList(std::vector<std::wstring> files, int start) {
    g.files = std::move(files);
    g.strip.SetItems(&g.files);
    g.cur = -1;
    g.rot = 0;
    g.gridScroll = 0;
    g.gridSel = (std::max)(0, (std::min)(start, (int)g.files.size() - 1));
    if (!g.files.empty()) {
        g.cur = (std::max)(0, (std::min)(start, (int)g.files.size() - 1));
        g.page = 0;
        g.fitMode = true;
        LoadCurrent();
    } else {
        LoadCurrent();
    }
}

bool CanonicalPath(const wchar_t* in, std::wstring& out) {
    wchar_t buf[4096];
    DWORD n = GetFullPathNameW(in, 4096, buf, nullptr);
    if (n == 0 || n >= 4096) return false;
    out = buf;
    return true;
}

void StartListThread(const std::wstring& target, bool isFolder) {
    g.listGen++;
    g.listPending = true;
    g.listWasEmpty = false; // a fresh scan owns the empty-state message
    auto* req = new ListReq{g.hwnd, g.listGen, target, isFolder};
    HANDLE h = CreateThread(nullptr, 0, ListThreadProc, req, 0, nullptr);
    if (h)
        CloseHandle(h);
    else
        delete req;
}

// Open a file or folder (command line, Ctrl+O, or drag & drop).
void OpenTarget(const wchar_t* rawPath) {
    std::wstring path;
    if (!CanonicalPath(rawPath, path)) return;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();

    MaybeCommitRotation();
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        AdoptFileList({}, 0);
        StartListThread(path, true);
    } else {
        // A video file that the engine can't play: say why, rather than a
        // generic "not supported". In an image-only build (opened via the
        // .sln / build.bat / MinGW) player_video_init_error() reports that
        // video wasn't compiled in; in a video build it reports the D3D
        // initialization failure.
        if (IsVideoFile(path) && !IsPlayableVideo(path)) {
            std::wstring msg = L"This copy of Media Gallery can't play video.\n\n";
            msg += player_video_init_error();
            msg += L"\n\nVideo playback is compiled only by the CMake build:\n"
                   L"    cmake --preset x64-release\n"
                   L"    cmake --build --preset x64-release\n\n"
                   L"The Visual Studio .sln, build.bat and MinGW builds are "
                   L"image-only by design. See BUILDING.md.";
            MessageBoxW(g.hwnd, msg.c_str(), L"Media Gallery",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (!IsSupportedImage(path) && !IsPlayableVideo(path)) {
            MessageBoxW(g.hwnd, L"This file type is not supported.", L"Media Gallery",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        // Show the picked file immediately; siblings arrive asynchronously.
        AdoptFileList({path}, 0);
        StartListThread(path, false);
    }
}

void OpenDialog() {
    std::wstring filter = OpenDialogFilter(g.player != nullptr);
    wchar_t file[4096] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = 4096;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) OpenTarget(file);
}

void HandleDrop(HDROP drop) {
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> paths;
    for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring p(len + 1, L'\0');
        DragQueryFileW(drop, i, &p[0], len + 1);
        p.resize(len);
        std::wstring canon;
        if (CanonicalPath(p.c_str(), canon)) paths.push_back(std::move(canon));
    }
    DragFinish(drop);
    if (paths.empty()) return;

    if (paths.size() == 1) {
        OpenTarget(paths[0].c_str());
        return;
    }
    // A dropped multi-selection becomes the navigation scope itself.
    std::vector<std::wstring> images;
    for (const auto& p : paths) {
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY) &&
            (IsSupportedImage(p) || IsPlayableVideo(p)))
            images.push_back(p);
    }
    if (images.empty()) {
        MessageBoxW(g.hwnd, L"No supported files in the dropped items.", L"Media Gallery",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::sort(images.begin(), images.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
              });
    MaybeCommitRotation();
    g.listGen++; // invalidate any in-flight folder scan
    g.listPending = false;
    AdoptFileList(std::move(images), 0);
}

void RotateView(int delta) {
    if (!g.disp) return;
    g.rot = ((g.rot + delta) % 4 + 4) % 4;
    g.fitMode = true;
    RebuildDisplayBitmap();
    UpdateTitle();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void SaveRotationNow() {
    if (g.rot == 0 || !g.disp || g.cur < 0 || g.savePending) return;
    const std::wstring& path = g.files[g.cur];
    ImageDecoder* dec = FindDecoder(path);
    if (!dec || !dec->CanSaveRotation(path, *g.disp)) {
        MessageBoxW(g.hwnd, L"Rotation can't be saved for this file type.", L"Media Gallery",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    g.savePending = true;
    g.worker.RequestSave(path, g.rot);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void DeleteAt(int index) {
    if (index < 0 || index >= (int)g.files.size()) return;
    const std::wstring path = g.files[index];

    const bool wasSlideshow = g.slideshow && !g.gridMode && !g.videoMode;

    // A playing engine holds the file open, which makes the recycle-bin
    // move fail silently — release it first.
    const bool closedVideo = index == g.cur && g.videoMode;
    if (closedVideo && g.player) player_close(g.player);

    IFileOperation* op = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_IFileOperation, reinterpret_cast<void**>(&op))))
        return;
    // The recycle-confirm dialog pumps a modal loop; silence the slideshow
    // timer so it can't re-enter navigation and shift items mid-delete.
    if (wasSlideshow) KillTimer(g.hwnd, kTimerSlideshow);
    bool deleted = false;
    op->SetOwnerWindow(g.hwnd);
    op->SetOperationFlags(FOF_ALLOWUNDO); // Recycle Bin, standard confirmations
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem,
                                              reinterpret_cast<void**>(&item)))) {
        if (SUCCEEDED(op->DeleteItem(item, nullptr)) &&
            SUCCEEDED(op->PerformOperations())) {
            BOOL aborted = FALSE;
            op->GetAnyOperationsAborted(&aborted);
            deleted = !aborted && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
        }
        item->Release();
    }
    op->Release();
    // LoadCurrent re-arms the slideshow timer from g.slideshow; on paths that
    // don't call it, restore the timer we killed above.
    auto rearmSlideshow = [&] {
        if (wasSlideshow && g.slideshow && !g.gridMode && !g.videoMode)
            SetTimer(g.hwnd, kTimerSlideshow, g.slideshowMs, nullptr);
    };
    if (!deleted) {
        if (closedVideo) LoadCurrent(); // reopen the video we closed above
        else rearmSlideshow();
        return;
    }

    // PerformOperations() pumps a modal shell dialog, during which a folder
    // scan's WM_APP_LIST can replace g.files (and g.cur). Re-resolve the row
    // to erase by path so we never index the stale/old vector out of bounds.
    index = -1;
    for (int i = 0; i < (int)g.files.size(); ++i)
        if (lstrcmpiW(g.files[i].c_str(), path.c_str()) == 0) { index = i; break; }
    if (index < 0) {
        // Already absent from the current listing (the scan dropped it too).
        if (closedVideo) LoadCurrent();
        else rearmSlideshow();
        return;
    }

    CacheRemovePath(path);
    g.strip.InvalidateThumb(path);
    if (index == g.cur) g.rot = 0;
    g.files.erase(g.files.begin() + index);
    if (g.files.empty()) {
        g.cur = -1;
        g.gridSel = 0;
        LoadCurrent();
        return;
    }
    if (index < g.cur) {
        g.cur--; // list shifted under the shown image; nothing to reload
        g.strip.SetCurrent(g.cur);
        UpdateTitle();
    } else if (index == g.cur) {
        g.cur = (std::min)(g.cur, (int)g.files.size() - 1);
        g.page = 0;
        g.fitMode = true;
        LoadCurrent();
    }
    g.gridSel = (std::max)(0, (std::min)(index, (int)g.files.size() - 1));
    if (g.gridMode) GridEnsureVisible();
    // The index<cur and index>cur branches above don't call LoadCurrent, so
    // re-arm the slideshow timer we killed (idempotent if LoadCurrent already
    // did on the index==cur path).
    rearmSlideshow();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void EditInPaint(int index) {
    if (index < 0 || index >= (int)g.files.size()) return;
    if (IsVideoFile(g.files[index])) return; // Paint can't open video files
    if (index == g.cur) {
        MaybeCommitRotation();
        UpdateTitle();
    }
    // Fixed executable, quoted path parameter — nothing user-controlled picks the app.
    std::wstring params = L"\"" + g.files[index] + L"\"";
    ShellExecuteW(g.hwnd, L"open", L"mspaint.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
}

void BeginInteractive() {
    g.interactive = true;
    SetTimer(g.hwnd, kTimerHq, 150, nullptr);
}

void ZoomAt(POINT anchor, double factor) {
    RECT rc = ClientRect();
    Layout L = ComputeLayout(rc);
    if (!L.valid) return;
    double oldScale = L.scale;
    double newScale = (std::max)(kMinZoom, (std::min)(kMaxZoom, oldScale * factor));
    if (std::fabs(newScale - oldScale) < 1e-9) return;
    double imgX = L.srcX + (anchor.x - L.dest.left) / oldScale;
    double imgY = L.srcY + (anchor.y - L.dest.top) / oldScale;
    imgX = (std::max)(0.0, (std::min)(imgX, (double)g.dispW));
    imgY = (std::max)(0.0, (std::min)(imgY, (double)g.dispH));
    g.fitMode = false;
    g.zoom = newScale;
    g.panX = imgX - (anchor.x - L.imgArea.left) / newScale;
    g.panY = imgY - (anchor.y - L.imgArea.top) / newScale;
    BeginInteractive();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void ZoomStep(double factor) {
    RECT rc = ClientRect();
    Layout L = ComputeLayout(rc);
    if (!L.valid) return;
    POINT center = {(L.imgArea.left + L.imgArea.right) / 2,
                    (L.imgArea.top + L.imgArea.bottom) / 2};
    ZoomAt(center, factor);
}

void ReloadIfChangedOnDisk() {
    if (g.cur < 0) return;
    if (g.videoMode) return; // don't restart playback over a timestamp change
    const std::wstring& path = g.files[g.cur];
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        // Deleted or renamed externally.
        CacheRemovePath(path);
        g.strip.InvalidateThumb(path);
        g.files.erase(g.files.begin() + g.cur);
        g.rot = 0;
        g.cur = g.files.empty() ? -1 : (std::min)(g.cur, (int)g.files.size() - 1);
        LoadCurrent();
        return;
    }
    if (CompareFileTime(&fad.ftLastWriteTime, &g.lastWrite) != 0) {
        CacheRemovePath(path);
        g.strip.InvalidateThumb(path);
        LoadCurrent(); // rot intentionally kept: user may be mid-rotation
    }
}

// ------------------------------------------------------------ message handlers

void OnDecoded(DecodeDone* d) {
    std::unique_ptr<DecodeDone> owner(d);
    std::shared_ptr<DecodedImage> img(d->img);
    const std::wstring key = CacheKey(d->path, d->page);
    if (img) CachePut(key, img);
    if (g.cur >= 0 && !g.videoMode && g.files[g.cur] == d->path && g.page == d->page) {
        g.decodePending = false;
        if (img) {
            g.disp = img;
            g.decodeFailed = false;
            RebuildDisplayBitmap();
            PrefetchNeighbors();
        } else {
            g.decodeFailed = true;
            // Distinguish the two commonest, cheaply-diagnosable causes so the
            // user isn't left with only the generic line.
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(d->path.c_str(), GetFileExInfoStandard, &fad) &&
                fad.nFileSizeLow == 0 && fad.nFileSizeHigh == 0)
                g.failReason = L"This file is empty (0 bytes).";
            else
                g.failReason =
                    L"This file couldn't be decoded — it may be corrupt or truncated.";
        }
        g.detailsDirty = true; // dimensions/page rows may have changed
        InvalidateRect(g.hwnd, nullptr, FALSE);
        UpdateTitle();
    }
}

void OnSaved(SaveDone* d) {
    std::unique_ptr<SaveDone> owner(d);
    g.savePending = false;
    CacheRemovePath(d->path);
    g.strip.InvalidateThumb(d->path);
    if (!d->ok) {
        MessageBoxW(g.hwnd, L"The rotation could not be saved.", L"Media Gallery",
                    MB_OK | MB_ICONWARNING);
    }
    if (g.cur >= 0 && g.files[g.cur] == d->path) {
        if (d->ok) g.rot = 0; // file now matches the rotated view
        LoadCurrent();
    } else {
        InvalidateRect(g.hwnd, nullptr, FALSE);
    }
}

void OnProbed(ProbeDone* d) {
    std::unique_ptr<ProbeDone> owner(d);
    if (!d->ok || !g.videoMode || g.cur < 0 || g.files[g.cur] != d->path) return;
    g.videoInfo = d->info;
    g.videoProbed = true;
    g.detailsDirty = true; // codec/track rows just arrived
    if (g.showDetails) {
        PositionVideoWindow(); // the panel gained rows; regrow the region hole
        InvalidateRect(g.hwnd, nullptr, FALSE);
    }
}

// Marshaled engine events (posted by OnPlayerEvent); stale generations are
// events from a media we already navigated away from.
void OnPlayerMessage(WPARAM evt, LPARAM gen) {
    if (!g.videoMode || static_cast<unsigned>(gen) != g.playerGen) return;
    switch (static_cast<PlayerEvent>(evt)) {
        case PLAYER_EVT_OPENED:
            g.videoOpened = true;   // the child window may show itself now
            g.detailsDirty = true;  // duration/position are valid now
            PositionVideoWindow();
            break;
        case PLAYER_EVT_ENDED:
            g.videoEnded = true;
            // A finished video stays put on its last frame; we never auto-advance
            // to the next item. Only an explicit slideshow cycles through media.
            if (g.slideshow) {
                if (g.files.size() > 1) {
                    SlideshowAdvance(); // shuffle-aware
                    return;
                }
                // Single-item slideshow: loop the video, or the mode would
                // silently die after one playthrough (flag on, no timer).
                VideoPlayPause(); // videoEnded restart path: seek 0 + play
                return;
            }
            break;
        case PLAYER_EVT_ERROR: {
            // Surface the engine's own diagnostic ("Cannot open file… <why>",
            // "No decodable streams", …) instead of only the generic line.
            // Grab it before close, which resets pipeline state.
            wchar_t err[512] = L"";
            if (g.player) player_last_error(g.player, err, 512);
            g.videoError = err;
            g.videoMode = false;
            g.decodeFailed = true;
            if (g.player) player_close(g.player);
            KillTimer(g.hwnd, kTimerVideo);
            // A slideshow was waiting on ENDED, which will never come. With
            // other items, dwell on the error then advance; a single failing
            // item has nowhere to go, so stop the slideshow instead of leaving
            // it "on" with no timer and no way to progress.
            if (g.slideshow) {
                if (g.files.size() > 1)
                    SetTimer(g.hwnd, kTimerSlideshow, g.slideshowMs, nullptr);
                else
                    StopSlideshow();
            }
            PositionVideoWindow();
            break;
        }
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void OnListDone(ListDone* d) {
    std::unique_ptr<ListDone> owner(d);
    if (d->gen != g.listGen) return; // superseded by a newer open
    g.listPending = false;
    // Keep the currently shown file selected if it's in the new list.
    int start = d->start;
    if (g.cur >= 0 && !g.files.empty()) {
        const std::wstring& shown = g.files[g.cur];
        for (size_t i = 0; i < d->files.size(); ++i) {
            if (lstrcmpiW(d->files[i].c_str(), shown.c_str()) == 0) {
                start = (int)i;
                break;
            }
        }
    }
    // Adopt without disturbing the already-decoding current image.
    std::wstring shownPath = (g.cur >= 0) ? g.files[g.cur] : L"";
    g.files = std::move(d->files);
    g.strip.SetItems(&g.files);
    // Distinguish "scan found nothing" from "nothing opened yet": the empty
    // state then explains itself instead of showing the fresh-launch hint.
    g.listWasEmpty = g.files.empty();
    if (g.files.empty()) {
        g.cur = -1;
        LoadCurrent();
        return;
    }
    start = (std::max)(0, (std::min)(start, (int)g.files.size() - 1));
    g.cur = start;
    g.gridSel = start;
    if (g.gridMode) GridEnsureVisible();
    g.strip.SetCurrent(g.cur);
    if (shownPath.empty() || lstrcmpiW(g.files[g.cur].c_str(), shownPath.c_str()) != 0) {
        g.page = 0;
        g.rot = 0;
        g.fitMode = true;
        LoadCurrent();
    } else {
        PrefetchNeighbors();
        UpdateTitle();
        InvalidateRect(g.hwnd, nullptr, FALSE);
    }
}

void OnCommand(WORD id) {
    switch (id) {
        case IDM_OPEN: OpenDialog(); break;
        case IDM_EXIT:
            if (g.slideshow) StopSlideshow(); // Esc backs out of the slideshow,
            else if (g.gridMode) ToggleGrid(); // then the grid, before closing
            else PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
            break;
        case IDM_EDIT_PAINT: EditInPaint(g.gridMode ? g.gridSel : g.cur); break;
        case IDM_DELETE: DeleteAt(g.gridMode ? g.gridSel : g.cur); break;
        case IDM_ROTATE_CW:
            if (!g.gridMode) RotateView(1);
            break;
        case IDM_ROTATE_CCW:
            if (!g.gridMode) RotateView(-1);
            break;
        case IDM_SAVE:
            if (!g.gridMode) SaveRotationNow();
            break;
        case IDM_DETAILS:
            g.showDetails = !g.showDetails;
            PositionVideoWindow(); // punch/clear the panel hole in the child
            InvalidateRect(g.hwnd, nullptr, FALSE);
            break;
        case IDM_FILMSTRIP:
            g.showFilmstrip = !g.showFilmstrip;
            PositionVideoWindow();
            InvalidateRect(g.hwnd, nullptr, FALSE);
            break;
        case IDM_GRID: ToggleGrid(); break;
        case IDM_ZOOMIN:
            if (g.gridMode) GridZoom(true);
            else ZoomStep(1.25);
            break;
        case IDM_ZOOMOUT:
            if (g.gridMode) GridZoom(false);
            else ZoomStep(1.0 / 1.25);
            break;
        case IDM_FIT:
            if (!g.gridMode) {
                g.fitMode = true;
                InvalidateRect(g.hwnd, nullptr, FALSE);
            }
            break;
        case IDM_NEXT:
            if (g.gridMode) GridMoveSel(1);
            else if (!g.files.empty()) NavigateTo(g.cur + 1);
            break;
        case IDM_PREV:
            if (g.gridMode) GridMoveSel(-1);
            else if (!g.files.empty()) NavigateTo(g.cur - 1);
            break;
        case IDM_PAGE_NEXT:
            if (g.gridMode) GridMoveSel(g.gridCols);
            else SetPage((int)g.page + 1);
            break;
        case IDM_PAGE_PREV:
            if (g.gridMode) GridMoveSel(-g.gridCols);
            else SetPage((int)g.page - 1);
            break;
        case IDM_PLAYPAUSE:
            if (!g.gridMode) VideoPlayPause();
            break;
        case IDM_SEEK_FWD:
            if (!g.gridMode) VideoSeekBy(10);
            break;
        case IDM_SEEK_BACK:
            if (!g.gridMode) VideoSeekBy(-10);
            break;
        case IDM_SLIDESHOW: ToggleSlideshow(); break;
        case IDM_SS_2S:
        case IDM_SS_5S:
        case IDM_SS_10S:
            g.slideshowMs = (id == IDM_SS_2S) ? 2000
                          : (id == IDM_SS_10S) ? 10000 : 5000;
            // Re-arm a running image slideshow at the new pace right away.
            if (g.slideshow && !g.gridMode && !g.videoMode)
                SetTimer(g.hwnd, kTimerSlideshow, g.slideshowMs, nullptr);
            break;
        case IDM_SS_SHUFFLE:
            g.slideshowShuffle = !g.slideshowShuffle;
            break;
        default: break;
    }
}

void OnInitMenuPopup(HMENU menu) {
    const bool haveImage = g.cur >= 0;
    const bool haveDisp = g.disp != nullptr;
    auto enable = [&](UINT id, bool on) {
        EnableMenuItem(menu, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED));
    };
    const bool haveAny = !g.files.empty();
    const bool gridSelImage = g.gridMode && g.gridSel >= 0 &&
                              g.gridSel < (int)g.files.size() &&
                              !IsVideoFile(g.files[g.gridSel]);
    // Not in video mode: Paint can't open a .mp4/.mkv, so the item would
    // silently no-op if the menu let it through.
    enable(IDM_EDIT_PAINT, g.gridMode ? gridSelImage : (haveDisp && !g.videoMode));
    enable(IDM_DELETE, g.gridMode ? haveAny : haveImage);
    enable(IDM_ROTATE_CW, haveDisp && !g.gridMode);
    enable(IDM_ROTATE_CCW, haveDisp && !g.gridMode);
    enable(IDM_SAVE, haveDisp && !g.gridMode && g.rot != 0 && !g.savePending);
    enable(IDM_ZOOMIN, g.gridMode || haveDisp);
    enable(IDM_ZOOMOUT, g.gridMode || haveDisp);
    enable(IDM_FIT, haveDisp && !g.gridMode);
    enable(IDM_NEXT, g.files.size() > 1);
    enable(IDM_PREV, g.files.size() > 1);
    enable(IDM_PAGE_NEXT, g.gridMode ? haveAny : (haveDisp && g.disp->pageCount > 1));
    enable(IDM_PAGE_PREV, g.gridMode ? haveAny : (haveDisp && g.disp->pageCount > 1));
    const bool video = g.videoMode && !g.gridMode;
    enable(IDM_PLAYPAUSE, video);
    enable(IDM_SEEK_FWD, video);
    enable(IDM_SEEK_BACK, video);
    enable(IDM_SLIDESHOW, haveAny);
    CheckMenuItem(menu, IDM_SLIDESHOW,
                  MF_BYCOMMAND | (g.slideshow ? MF_CHECKED : MF_UNCHECKED));
    const UINT dwell = (g.slideshowMs == 2000) ? IDM_SS_2S
                     : (g.slideshowMs == 10000) ? IDM_SS_10S : IDM_SS_5S;
    CheckMenuRadioItem(menu, IDM_SS_2S, IDM_SS_10S, dwell, MF_BYCOMMAND);
    CheckMenuItem(menu, IDM_SS_SHUFFLE,
                  MF_BYCOMMAND | (g.slideshowShuffle ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_DETAILS,
                  MF_BYCOMMAND | (g.showDetails ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_FILMSTRIP,
                  MF_BYCOMMAND | (g.showFilmstrip ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu, IDM_GRID,
                  MF_BYCOMMAND | (g.gridMode ? MF_CHECKED : MF_UNCHECKED));
}

void OnLButtonDown(POINT pt) {
    RECT rc = ClientRect();
    if (g.gridMode) {
        GridLayout gl = ComputeGrid(rc);
        int idx = GridHitTest(pt, gl);
        if (idx >= 0) OpenFromGrid(idx); // click switches to the image
        return;
    }
    Layout L = ComputeLayout(rc);
    UiRects ui = ComputeUiRects(L.imgArea);

    if (ui.videoVisible) {
        if (PtInRect(&ui.playBtn, pt)) { VideoPlayPause(); return; }
        // The whole bar height around the thin seek line is clickable.
        RECT seekHit = {ui.seekBar.left, ui.videoBar.top, ui.seekBar.right, ui.videoBar.bottom};
        if (PtInRect(&seekHit, pt)) {
            const double dur = g.player ? player_duration(g.player) : 0.0;
            if (dur > 0) {
                const double w = (std::max)(1L, ui.seekBar.right - ui.seekBar.left);
                const double target = dur * (pt.x - ui.seekBar.left) / w;
                SeekFlash(target);
                player_seek_to(g.player, target);
                g.videoEnded = false;
                InvalidateRect(g.hwnd, nullptr, FALSE);
            }
            return;
        }
        if (PtInRect(&ui.videoBar, pt)) return; // dead space on the bar
    }

    if (ui.barVisible && PtInRect(&ui.rotCcw, pt)) { RotateView(-1); return; }
    if (ui.barVisible && PtInRect(&ui.rotCw, pt)) { RotateView(1); return; }
    if (ui.barVisible && ui.saveVisible && PtInRect(&ui.save, pt)) { SaveRotationNow(); return; }
    if (ui.pageVisible && PtInRect(&ui.pagePrev, pt)) { SetPage((int)g.page - 1); return; }
    if (ui.pageVisible && PtInRect(&ui.pageNext, pt)) { SetPage((int)g.page + 1); return; }

    if (g.showFilmstrip && PtInRect(&L.stripRc, pt)) {
        int idx = g.strip.HitTest(pt, L.stripRc);
        if (idx >= 0 && idx != g.cur) NavigateTo(idx);
        return;
    }

    if (L.valid && PtInRect(&L.imgArea, pt)) {
        // Pan only when the image overflows the viewport.
        const double areaW = L.imgArea.right - L.imgArea.left;
        const double areaH = L.imgArea.bottom - L.imgArea.top;
        if (g.dispW * L.scale > areaW || g.dispH * L.scale > areaH) {
            g.dragging = true;
            g.dragStart = pt;
            g.dragPanX = g.panX;
            g.dragPanY = g.panY;
            SetCapture(g.hwnd);
        }
    }
}

void OnMouseMove(POINT pt) {
    if (!g.dragging) return;
    RECT rc = ClientRect();
    Layout L = ComputeLayout(rc);
    if (!L.valid) return;
    g.panX = g.dragPanX - (pt.x - g.dragStart.x) / L.scale;
    g.panY = g.dragPanY - (pt.y - g.dragStart.y) / L.scale;
    BeginInteractive();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void OnMouseWheel(POINT screenPt, int delta, bool ctrl) {
    POINT pt = screenPt;
    ScreenToClient(g.hwnd, &pt);
    RECT rc = ClientRect();
    if (g.gridMode) {
        if (ctrl) {
            GridZoom(delta > 0);
        } else {
            g.gridScroll -= delta; // clamped in ComputeGrid
            InvalidateRect(g.hwnd, nullptr, FALSE);
        }
        return;
    }
    Layout L = ComputeLayout(rc);
    if (g.showFilmstrip && PtInRect(&L.stripRc, pt)) {
        g.strip.Scroll(delta);
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return;
    }
    if (g.videoMode) {
        // Wheel over the video adjusts volume (forwarded by VideoWndProc too).
        if (g.player) player_volume_step(g.player, delta / 120);
        return;
    }
    ZoomAt(pt, std::pow(1.25, delta / 120.0));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc, ClientRect());
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            PositionVideoWindow();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            // DPI-scaled: at 200% the chrome (filmstrip + transport + buttons)
            // needs twice the room, or the image area collapses to a sliver.
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize = {Scaled(480), Scaled(360)};
            return 0;
        }
        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;
        case WM_INITMENUPOPUP:
            OnInitMenuPopup(reinterpret_cast<HMENU>(wParam));
            return 0;
        case WM_LBUTTONDOWN:
            OnLButtonDown({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_LBUTTONUP:
            if (g.dragging) {
                g.dragging = false;
                ReleaseCapture();
            }
            return 0;
        case WM_MOUSEWHEEL:
            OnMouseWheel({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)},
                         GET_WHEEL_DELTA_WPARAM(wParam),
                         (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0);
            return 0;
        case WM_KEYDOWN:
            if (g.gridMode && wParam == VK_RETURN) {
                OpenFromGrid(g.gridSel);
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        case WM_TIMER:
            if (wParam == kTimerHq) {
                KillTimer(hwnd, kTimerHq);
                g.interactive = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wParam == kTimerVideo && g.videoMode && !g.gridMode) {
                // Repaint only the transport bar; a full-window invalidate on
                // every tick would redraw thumbnails, details and letterbox
                // for a change confined to the seek fill and time text. Grid
                // mode draws no bar at all — repainting there would thrash
                // the thumbnail grid at 10Hz for nothing.
                Layout L = ComputeLayout(ClientRect());
                RECT bar = {L.imgArea.left, L.imgArea.bottom - Scaled(kVideoBarH),
                            L.imgArea.right, L.imgArea.bottom};
                InvalidateRect(hwnd, &bar, FALSE);
            } else if (wParam == kTimerSlideshow) {
                if (g.slideshow && !g.gridMode && !g.videoMode) SlideshowAdvance();
            }
            return 0;
        case WM_DROPFILES:
            HandleDrop(reinterpret_cast<HDROP>(wParam));
            return 0;
        case WM_ACTIVATEAPP:
            if (wParam) ReloadIfChangedOnDisk();
            return 0;
        case WM_DPICHANGED: {
            g.dpi = HIWORD(wParam);
            g.strip.SetDpi(g.dpi);
            const RECT* r = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                         r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            PositionVideoWindow(); // the scaled transport strip moved
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP_DECODED:
            OnDecoded(reinterpret_cast<DecodeDone*>(lParam));
            return 0;
        case WM_APP_SAVED:
            OnSaved(reinterpret_cast<SaveDone*>(lParam));
            return 0;
        case WM_APP_LIST:
            OnListDone(reinterpret_cast<ListDone*>(lParam));
            return 0;
        case WM_APP_THUMB: {
            auto* r = reinterpret_cast<Filmstrip::ThumbResult*>(lParam);
            g.strip.OnThumbReady(r);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_APP_PLAYER:
            OnPlayerMessage(wParam, lParam);
            return 0;
        case WM_APP_PROBED:
            OnProbed(reinterpret_cast<ProbeDone*>(lParam));
            return 0;
        case WM_CLOSE:
            // Silence the slideshow first: its timer must not fire into the
            // save prompt's modal loop and reset g.rot before we read it.
            KillTimer(hwnd, kTimerSlideshow);
            g.slideshow = false;
            // Last chance to persist an unsaved rotation (synchronous: we're
            // exiting). Skip if a save for this rotation is already queued —
            // prompting again would apply the same quarter-turn a second time
            // and race the worker's ReplaceFileW on the same file.
            if (g.rot != 0 && g.disp && g.cur >= 0 && !g.savePending) {
                // By value: MessageBoxW pumps a modal loop that can deliver a
                // WM_APP_LIST and free the string this would otherwise alias.
                const std::wstring path = g.files[g.cur];
                ImageDecoder* dec = FindDecoder(path);
                if (dec && dec->CanSaveRotation(path, *g.disp)) {
                    std::wstring m = L"Save the rotation to \"";
                    m += PathFindFileNameW(path.c_str());
                    m += L"\" before closing?";
                    if (MessageBoxW(hwnd, m.c_str(), L"Media Gallery",
                                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                        dec->SaveRotation(path, g.rot);
                        SetCursor(old);
                    }
                }
                g.rot = 0;
            }
            if (g.player) player_close(g.player); // detach D3D before the child dies
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// Video child: D3D renders here; forward input to the parent so its hit
// testing (transport bar, wheel volume) and accelerators keep working.
LRESULT CALLBACK VideoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            MapWindowPoints(hwnd, GetParent(hwnd), &pt, 1);
            return SendMessageW(GetParent(hwnd), msg, wParam, MAKELPARAM(pt.x, pt.y));
        }
        case WM_MOUSEWHEEL: // lParam is already in screen coordinates
            return SendMessageW(GetParent(hwnd), msg, wParam, lParam);
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE; // keep focus (and accelerators) on the parent
        case WM_ERASEBKGND:
            return 1; // D3D owns this surface
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        return 1;
    srand(GetTickCount()); // slideshow shuffle order varies run to run
    InitDecoders();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MAINMENU);
    wc.lpszClassName = L"MediaGalleryWnd";
    if (!RegisterClassExW(&wc)) return 1;

    // WS_CLIPCHILDREN: Paint() BitBlts the whole client; without it every
    // repaint would stomp the D3D video child.
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Media Gallery",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             1100, 800, nullptr, nullptr, hInst, nullptr);
    if (!g.hwnd) return 1;
    g.dpi = (int)GetDpiForWindow(g.hwnd);
    g.accel = LoadAcceleratorsW(hInst, MAKEINTRESOURCEW(IDR_ACCEL));
    DragAcceptFiles(g.hwnd, TRUE);

    // Video child + engine. A null player (stub build, or D3D11 init
    // failure) simply leaves video files unsupported.
    WNDCLASSEXW vc{};
    vc.cbSize = sizeof(vc);
    vc.lpfnWndProc = VideoWndProc;
    vc.hInstance = hInst;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    vc.lpszClassName = L"MediaGalleryVideo";
    if (RegisterClassExW(&vc))
        g.videoWnd = CreateWindowExW(0, vc.lpszClassName, nullptr, WS_CHILD, 0, 0,
                                     1, 1, g.hwnd, nullptr, hInst, nullptr);
    if (g.videoWnd) g.player = player_create(g.videoWnd);

    g.worker.Start(g.hwnd);
    g.strip.Start(g.hwnd, WM_APP_THUMB);
    g.strip.SetDpi(g.dpi);
    g.strip.SetItems(&g.files);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        if (argc > 1) OpenTarget(argv[1]);
        LocalFree(argv);
    }

    ShowWindow(g.hwnd, nCmdShow);
    UpdateTitle();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g.accel && TranslateAcceleratorW(g.hwnd, g.accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g.worker.Stop();
    g.strip.Stop();
    if (g.player) {
        player_destroy(g.player);
        g.player = nullptr;
    }
    if (g.displayBmp) DeleteObject(g.displayBmp);
    if (g.fontUi) DeleteObject(g.fontUi);
    if (g.fontSym) DeleteObject(g.fontSym);
    if (g.fontBig) DeleteObject(g.fontBig);
    g.cache.clear();
    g.disp.reset();
    ShutdownDecoders();
    CoUninitialize();
    return (int)msg.wParam;
}
