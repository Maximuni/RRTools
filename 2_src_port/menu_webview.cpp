/* menu_webview.cpp - WebView2 menu host and settings bridge. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "WebView2.h"
#include "menu_bridge.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "WebView2LoaderStatic.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

static BR_Config g_cfg;
static int g_cfg_init = 0;

static void init_defaults(void) {
    if (g_cfg_init) return;
    g_cfg_init = 1;
    memset(&g_cfg, 0, sizeof(g_cfg));
#define BR_SET_DEFAULT(field, initial, minimum, maximum) g_cfg.field = (initial);
    BR_SETTINGS(BR_SET_DEFAULT)
#undef BR_SET_DEFAULT
    g_cfg.menu_open = 1;

    BR_Color defs[BR_COLOR_COUNT] = {
        { { 255,  70,  70 }, 0 }, /*  0 Enemy default            */
        { {  80, 220, 120 }, 0 }, /*  1 Party / Ally (F2)        */
        { { 255, 255, 255 }, 0 }, /*  2 Skeleton                 */
        { { 255, 220,  60 }, 0 }, /*  3 Head dot                 */
        { { 255,  80, 110 }, 0 }, /*  4 3D Chams                 */
        { { 150, 150, 155 }, 0 }, /*  5 Downed                   */
        { { 245, 245, 245 }, 0 }, /*  6 Name                     */
        { { 255, 195,  75 }, 0 }, /*  7 Weapon                   */
        { { 185, 210, 240 }, 0 }, /*  8 Distance                 */
        { {  80, 185, 255 }, 0 }, /*  9 Armor                    */
        { { 160, 115, 255 }, 0 }, /* 10 Hiding Spot              */
        { { 255, 215,  90 }, 0 }, /* 11 Snapline                 */
        { { 255,  68,  88 }, 0 }, /* 12 Enemy Visible (LOS)      */
        { {  70, 165, 255 }, 0 }, /* 13 Enemy Behind Wall        */
        { { 255, 225, 120 }, 0 }, /* 14 Aim FOV Circle           */
        { { 255, 255, 255 }, 0 },
        { { 255, 255, 255 }, 0 },
        { { 255, 255, 255 }, 0 },
    };
    memcpy(g_cfg.color, defs, sizeof(defs));
}

extern "C" BR_Config* br_get(void) {
    init_defaults();
    return &g_cfg;
}

static HWND                    g_host     = NULL;
static HANDLE                  g_thread   = NULL;
static DWORD                   g_tid      = 0;
static volatile LONG           g_want_vis = 1;
static volatile LONG           g_running  = 0;
static int                     g_last_sp  = -1;
static ICoreWebView2*          g_wv       = NULL;
static ICoreWebView2Controller* g_ctrl    = NULL;

#define WM_BR_SHOW   (WM_APP + 1)
#define WM_BR_TOGGLE (WM_APP + 2)

static void update_stream_proof(HWND hwnd) {
    if (!hwnd) return;
    int sp = br_get()->stream_proof ? 1 : 0;
    if (sp != g_last_sp) {
        g_last_sp = sp;
        SetWindowDisplayAffinity(hwnd, sp ? WDA_EXCLUDEFROMCAPTURE : 0);
    }
}

static void push_state(void) {
    if (!g_wv) return;
    BR_Config* b = br_get();
    char buf[4096];
    int n = snprintf(buf, sizeof(buf),
        "window.__setState && window.__setState({"
        "enabled:%d,show_box:%d,box_style:%d,show_skeleton:%d,show_name:%d,show_armor:%d,show_bars:%d,"
        "show_distance:%d,show_weapon:%d,show_head:%d,show_self:%d,show_clan:%d,show_police:%d,party_filter:%d,"
        "cham_on:%d,vischeck_colors:%d,snaplines_on:%d,show_hiding_spots:%d,stream_proof:%d,"
        "max_distance:%d,skel_distance:%d,line_w:%d,"
        "radar_on:%d,radar_rotate:%d,radar_range:%d,map_key:%d,"
        "aa_on:%d,aa_show_fov:%d,aa_fov_px:%d,aa_key:%d,aa_smooth:%d,aa_aim_cal:%d,"
        "aa_dist:%d,aa_target_clan:%d,aa_target_police:%d,aa_bone:%d,aa_los_on:%d,aa_closest_first:%d,"
        "tb_on:%d,tb_key:%d,tb_delay_ms:%d,tb_fov_px:%d,"
        "show_loot:%d,show_loot_nodes:%d,loot_tier_none:%d,loot_tier_common:%d,"
        "loot_tier_uncommon:%d,loot_tier_rare:%d,loot_tier_epic:%d,"
        "loot_tier_legendary:%d,loot_tier_exotic:%d,vehicle_chams:%d,"
        "show_containers:%d,show_doors:%d,show_dropped_loot:%d,show_vehicle_loot:%d,"
        "radar_world:%d,show_crouch:%d,show_health:%d,show_vehicle_markers:%d,show_vehicle_occupants:%d, instant_loot:%d,"
        "telem_targets:%d,telem_cham_n:%d,telem_los_state:%d,telem_game_pid:%d,"
        "colors:[",
        b->enabled, b->show_box, b->box_style, b->show_skeleton, b->show_name, b->show_armor, b->show_bars,
        b->show_distance, b->show_weapon, b->show_head, b->show_self, b->show_clan, b->show_police, b->party_filter,
        b->cham_on, b->vischeck_colors, b->snaplines_on, b->show_hiding_spots, b->stream_proof,
        b->max_distance, b->skel_distance, b->line_w,
        b->radar_on, b->radar_rotate, b->radar_range, b->map_key,
        b->aa_on, b->aa_show_fov, b->aa_fov_px, b->aa_key, b->aa_smooth, b->aa_aim_cal,
        b->aa_dist, b->aa_target_clan, b->aa_target_police, b->aa_bone, b->aa_los_on, b->aa_closest_first,
        b->tb_on, b->tb_key, b->tb_delay_ms, b->tb_fov_px,
        b->show_loot, b->show_loot_nodes, b->loot_tier_none, b->loot_tier_common,
        b->loot_tier_uncommon, b->loot_tier_rare, b->loot_tier_epic,
        b->loot_tier_legendary, b->loot_tier_exotic, b->vehicle_chams,
        b->show_containers, b->show_doors, b->show_dropped_loot, b->show_vehicle_loot,
        b->radar_world, b->show_crouch, b->show_health, b->show_vehicle_markers, b->show_vehicle_occupants, b->instant_loot,
        b->telem_targets, b->telem_cham_n, b->telem_los_state, b->telem_game_pid);
    for (int i = 0; i < BR_COLOR_COUNT && n < (int)sizeof(buf) - 32; ++i) {
        n += snprintf(buf + n, sizeof(buf) - n, "[%d,%d,%d]%s",
            b->color[i].rgb[0], b->color[i].rgb[1], b->color[i].rgb[2],
            i + 1 < BR_COLOR_COUNT ? "," : "");
    }
    snprintf(buf + n, sizeof(buf) - n, "]})");
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, -1, NULL, 0);
    wchar_t* wbuf = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (wbuf) {
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, wlen);
        g_wv->ExecuteScript(wbuf, NULL);
        free(wbuf);
    }
}

static bool json_str(const char* j, const char* key, char* out, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char* p = strstr(j, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return true;
}
static bool json_int(const char* j, const char* key, int* out) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* p = strstr(j, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '"') p++;
    char* end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = (int)v;
    return true;
}

static void handle_msg(const char* j) {
    BR_Config* b = br_get();
    char type[32] = {0};
    if (!json_str(j, "type", type, sizeof(type))) return;

    if (!strcmp(type, "ready")) {
        push_state();
        return;
    }
    if (!strcmp(type, "drag")) {
        if (g_host) {
            ReleaseCapture();
            SendMessageW(g_host, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        return;
    }
    if (!strcmp(type, "hide")) {
        b->menu_open = 0;
        InterlockedExchange(&g_want_vis, 0);
        if (g_host) ShowWindow(g_host, SW_HIDE);
        return;
    }
    if (!strcmp(type, "set")) {
        char k[48] = {0}; int v = 0;
        if (json_str(j, "k", k, sizeof(k)) && json_int(j, "v", &v)) {
            if (br_config_set(b, k, v)) {
                if (!strcmp(k, "stream_proof") && g_host)
                    update_stream_proof(g_host);
                br_config_save();
            }
        }
        return;
    }
    if (!strcmp(type, "color")) {
        int i = -1, r = 0, g = 0, bl = 0;
        if (json_int(j, "i", &i) && i >= 0 && i < BR_COLOR_COUNT &&
            json_int(j, "r", &r) && json_int(j, "g", &g) && json_int(j, "b", &bl)) {
            b->color[i].rgb[0] = (unsigned char)(r < 0 ? 0 : (r > 255 ? 255 : r));
            b->color[i].rgb[1] = (unsigned char)(g < 0 ? 0 : (g > 255 ? 255 : g));
            b->color[i].rgb[2] = (unsigned char)(bl < 0 ? 0 : (bl > 255 ? 255 : bl));
            br_config_save();
        }
        return;
    }
}

template <class T> struct ComBox : public T {
    LONG refs = 1;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r;
    }
};

struct MsgHandler : ComBox<ICoreWebView2WebMessageReceivedEventHandler> {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == IID_IUnknown || id == IID_ICoreWebView2WebMessageReceivedEventHandler) { *out = this; AddRef(); return S_OK; }
        *out = NULL; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) override {
        LPWSTR raw = NULL;
        if (SUCCEEDED(a->TryGetWebMessageAsString(&raw)) && raw) {
            int n = WideCharToMultiByte(CP_UTF8, 0, raw, -1, NULL, 0, NULL, NULL);
            char* s = (char*)malloc(n);
            if (s) {
                WideCharToMultiByte(CP_UTF8, 0, raw, -1, s, n, NULL, NULL);
                handle_msg(s);
                free(s);
            }
            CoTaskMemFree(raw);
        }
        return S_OK;
    }
};

static void find_menu_html(wchar_t* out, size_t cap) {
    GetModuleFileNameW(NULL, out, (DWORD)cap);
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(out, cap, L"menu.html");
}

struct CtrlDone : ComBox<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == IID_IUnknown || id == IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler) { *out = this; AddRef(); return S_OK; }
        *out = NULL; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* c) override {
        if (FAILED(hr) || !c) return S_OK;
        g_ctrl = c; g_ctrl->AddRef();
        g_ctrl->get_CoreWebView2(&g_wv);

        ICoreWebView2Controller2* c2 = NULL;
        if (SUCCEEDED(g_ctrl->QueryInterface(__uuidof(ICoreWebView2Controller2), (void**)&c2)) && c2) {
            COREWEBVIEW2_COLOR bg = { 0, 14, 15, 22 };
            c2->put_DefaultBackgroundColor(bg);
            c2->Release();
        }
        ICoreWebView2Settings* st = NULL;
        if (SUCCEEDED(g_wv->get_Settings(&st)) && st) {
            st->put_AreDefaultContextMenusEnabled(FALSE);
            st->put_AreDevToolsEnabled(FALSE);
            st->put_IsStatusBarEnabled(FALSE);
            st->put_IsZoomControlEnabled(FALSE);
            st->Release();
        }
        RECT rc; GetClientRect(g_host, &rc);
        g_ctrl->put_Bounds(rc);
        g_ctrl->put_IsVisible(TRUE);

        EventRegistrationToken tok;
        g_wv->add_WebMessageReceived(new MsgHandler(), &tok);

        wchar_t html[MAX_PATH], url[MAX_PATH + 16];
        find_menu_html(html, MAX_PATH);
        for (wchar_t* p = html; *p; ++p) if (*p == L'\\') *p = L'/';
        swprintf_s(url, MAX_PATH + 16, L"file:///%s", html);
        g_wv->Navigate(url);
        return S_OK;
    }
};

struct EnvDone : ComBox<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == IID_IUnknown || id == IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler) { *out = this; AddRef(); return S_OK; }
        *out = NULL; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* env) override {
        if (SUCCEEDED(hr) && env && g_host)
            env->CreateCoreWebView2Controller(g_host, new CtrlDone());
        return S_OK;
    }
};

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        if (g_ctrl) { RECT rc; GetClientRect(h, &rc); g_ctrl->put_Bounds(rc); }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (w == 7) {
            update_stream_proof(h);
            if (IsWindowVisible(h)) push_state();
        }
        return 0;
    case WM_BR_SHOW: {
        int show = (int)w;
        br_get()->menu_open = show ? 1 : 0;
        if (show) {
            update_stream_proof(h);
            ShowWindow(h, SW_SHOW);
            SetForegroundWindow(h);
            if (g_ctrl) g_ctrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            push_state();
        } else {
            ShowWindow(h, SW_HIDE);
        }
        return 0;
    }
    case WM_BR_TOGGLE: {
        int show = !IsWindowVisible(h);
        PostMessageW(h, WM_BR_SHOW, (WPARAM)show, 0);
        return 0;
    }
    case WM_CLOSE:
        PostMessageW(h, WM_BR_SHOW, 0, 0);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 7);
        if (g_ctrl) { g_ctrl->Close(); g_ctrl->Release(); g_ctrl = NULL; }
        if (g_wv)   { g_wv->Release();   g_wv = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI MenuThread(LPVOID) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = HostProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(14, 15, 22));
    wc.lpszClassName = L"CoreRR_MenuHost";
    RegisterClassExW(&wc);

    int W = 740, H = 640;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int x = (sx - W) / 2;
    int y = (sy - H) / 2;

    DWORD flags = WS_POPUP | (InterlockedCompareExchange(&g_want_vis, 0, 0) ? WS_VISIBLE : 0);
    g_host = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"CoreRR_MenuHost", L"CoreRR", flags,
        x, y, W, H, NULL, NULL, wc.hInstance, NULL);
    update_stream_proof(g_host);

    wchar_t dataDir[MAX_PATH];
    GetTempPathW(MAX_PATH, dataDir);
    wcscat_s(dataDir, MAX_PATH, L"CoreRR_WV2");
    CreateCoreWebView2EnvironmentWithOptions(NULL, dataDir, NULL, new EnvDone());

    SetTimer(g_host, 7, 1200, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    InterlockedExchange(&g_running, 0);
    return 0;
}

extern "C" void br_start_menu(void) {
    init_defaults();
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return;
    g_thread = CreateThread(NULL, 0, MenuThread, NULL, 0, &g_tid);
}

extern "C" void br_stop_menu(void) {
    if (!g_host) return;
    PostMessageW(g_host, WM_DESTROY, 0, 0);
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    g_host = NULL;
}

extern "C" void br_set_menu_visible(int show) {
    init_defaults();
    g_cfg.menu_open = show ? 1 : 0;
    InterlockedExchange(&g_want_vis, show ? 1 : 0);
    if (g_host) PostMessageW(g_host, WM_BR_SHOW, (WPARAM)(show ? 1 : 0), 0);
}

extern "C" void br_request_toggle(void) {
    init_defaults();
    if (!g_host) {
        int next = !g_cfg.menu_open;
        g_cfg.menu_open = next;
        InterlockedExchange(&g_want_vis, next);
        return;
    }
    PostMessageW(g_host, WM_BR_TOGGLE, 0, 0);
}