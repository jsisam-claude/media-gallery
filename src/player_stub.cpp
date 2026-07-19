// No-op implementation of the video engine API (player.h) for builds
// without video playback (MEDIAGALLERY_VIDEO=OFF, build.bat, the .vcxproj
// and the MinGW Makefile). player_create() returns null, which the app
// treats as "video files are not supported" — same behavior as before the
// engine existed, with no #ifdef in the gallery code.
#include "player.h"

Player* player_create(HWND) { return nullptr; }
void player_destroy(Player*) {}
void player_set_event_callback(Player*, PlayerEventFn, void*) {}

bool player_open(Player*, const wchar_t*) { return false; }
void player_close(Player*) {}
bool player_has_media(Player*) { return false; }
bool player_media_ended(Player*) { return false; }
bool player_is_buffering(Player*) { return false; }

void player_toggle_pause(Player*) {}
bool player_is_paused(Player*) { return false; }
void player_seek_rel(Player*, double) {}
void player_seek_to(Player*, double) {}
void player_volume_step(Player*, int) {}
void player_volume_set(Player*, float) {}
float player_volume(Player*) { return 1.0f; }
void player_set_mute(Player*, bool) {}
bool player_is_muted(Player*) { return false; }

int player_cycle_audio(Player*) { return 0; }
int player_cycle_subtitle(Player*) { return 0; }

void player_notify_resize(Player*) {}
double player_position(Player*) { return 0; }
double player_duration(Player*) { return 0; }

void player_last_error(Player*, wchar_t* buf, size_t buflen) {
    if (buf && buflen) buf[0] = L'\0';
}

const wchar_t* player_video_init_error(void) {
    return L"video playback is not built into this binary";
}

bool player_probe(const wchar_t*, PlayerMediaInfo* info) {
    if (info) *info = PlayerMediaInfo{};
    return false;
}

bool player_extract_thumb(const wchar_t*, int, int, uint8_t*, int* out_w, int* out_h) {
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    return false;
}
