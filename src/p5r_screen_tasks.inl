// [SCREEN] Game menus are classified from GFD's own running task names (the camp/pause menu,
// shops, calendar...). Read-only walk of the task lists, guarded reads, present thread only.
// Every task added/removed is logged, so new menus can be classified from evidence.
std::atomic<bool> g_screenMode{false};
char g_screenTask[64] = {};
struct ScreenRules { bool enabled = true; std::vector<std::string> include, exclude; };
ScreenRules g_screenRules;
bool g_screenRulesLoaded = false;
std::vector<std::string> g_taskNames;

bool TaskNameGuarded(uintptr_t name, char (&text)[80]) {
    uintptr_t chars = 0; int length = 0; uint32_t flags = 0; text[0] = 0;
    if (name < 0x10000 || name > 0x7FFFFFFFFFFF) return false;
    if (!Rd(name, flags) || !Rd(name + 8, chars) || chars < 0x10000 || chars > 0x7FFFFFFFFFFF || !Rd(name + 16, length)) return false;
    if (!(flags & 2) || length <= 0) {
        char buffer[80] = {};
        if (!SafeRead((const void*)chars, buffer, 79)) return false;
        for (int i = 0; i < 79; ++i) {
            unsigned char c = (unsigned char)buffer[i];
            if (!c) { text[i] = 0; return i > 0; }
            if (c < 32 || c > 126) return false;
            text[i] = (char)c;
        }
        return false;
    }
    if (length > 78 || !SafeRead((const void*)chars, text, length)) return false;
    text[length] = 0;
    for (int i = 0; i < length; ++i) if ((unsigned char)text[i] < 32 || (unsigned char)text[i] > 126) return false;
    return true;
}

void SplitRules(const wchar_t* text, std::vector<std::string>& out) {
    out.clear(); std::string item;
    for (const wchar_t* p = text;; ++p) {
        if (*p == L',' || *p == 0) {
            while (!item.empty() && item.back() == ' ') item.pop_back();
            size_t start = item.find_first_not_of(' ');
            if (start != std::string::npos) out.push_back(item.substr(start));
            item.clear();
            if (!*p) break;
        } else if (*p >= 32 && *p < 127) item += (char)*p;
    }
}

// Rules: "NAME" exact, "PREFIX*" prefix. Defaults come from task names in P5R.exe; the
// [Screen] section of p5r_camera.ini can replace them without a rebuild.
void LoadScreenRules() {
    if (g_screenRulesLoaded || !CameraControls::iniPath[0]) return;
    g_screenRulesLoaded = true;
    wchar_t text[2048] = {};
    g_screenRules.enabled = GetPrivateProfileIntW(L"Screen", L"Enabled", 1, CameraControls::iniPath) != 0;
    GetPrivateProfileStringW(L"Screen", L"Tasks", L"CAMP_MAIN,CMP_*,FCL_*,fld_start_menu,save,load,system_save_data_UI,fld_r1_menu,fld_square_menu,TITLE_DRAW,MOVIE_DRAW", text, 2048, CameraControls::iniPath);
    SplitRules(text, g_screenRules.include);
    GetPrivateProfileStringW(L"Screen", L"Ignore", L"CMP_BG,CMP_BG_CHARA_TEX_PROC", text, 2048, CameraControls::iniPath);
    SplitRules(text, g_screenRules.exclude);
    if (g_log) {
        std::string inc, exc;
        for (auto& s : g_screenRules.include) inc += s + " ";
        for (auto& s : g_screenRules.exclude) exc += s + " ";
        g_log("[screen] rules enabled=%d tasks=[ %s] ignore=[ %s]", (int)g_screenRules.enabled, inc.c_str(), exc.c_str());
    }
}

bool RuleMatch(const std::string& rule, const char* name) {
    if (!rule.empty() && rule.back() == '*') return strncmp(name, rule.c_str(), rule.size() - 1) == 0;
    return rule == name;
}
// [FLATSCREEN] title, movies and title Load Game are the flat screen.
static const char* const kFlatTasks[] = { "load", "TITLE_DRAW", "MOVIE_DRAW", "rail_map" };
bool IsFlatTask(const char* name) {
    if (!name) return false;
    for (const char* t : kFlatTasks) if (!strcmp(t, name)) return true;
    return false;
}
// [SOLIDPANEL] 10-07: Save, the network page (what others did) and loading screens: the docked VR
// panel with a solid black backing (intended: black background with the menu in front of it; the flat
// screen as a small mono rectangle was wrong). They win over other screen tasks (Save runs under
// fld_start_menu).
// 10-08: flat (Load Game style) or this black panel is the player's choice: Insert > HUD > "Save/loading as flat screen"
// 10-08 log: "place pict draw" (location card) outlives the load by up to ~4.7 s while Joker already moves: not a loading screen
static const char* const kSolidTasks[] = { "save", "NET_SCR_PROC", "drawVoiceThiefProc", "fld_scn_change", "cldDayChange", "Time Change Effect" };   // 10-08: + day change; 10-09: + time of day change
bool IsSolidTask(const char* name) {
    if (!name) return false;
    for (const char* t : kSolidTasks) if (!strcmp(t, name)) return true;
    return false;
}
bool IsScreenTask(const char* name) {
    for (auto& r : g_screenRules.exclude) if (RuleMatch(r, name)) return false;
    for (auto& r : g_screenRules.include) if (RuleMatch(r, name)) return true;
    return false;
}

// [UIDUMP] 2026-10-06: reported issues were battle UI missing/cut, a black shape during field chatter that
// never goes, and menu transparency issues. One run, no keys: when a UI-related task appears (or a
// message-like one ends), dump the HUD panel (+alpha) and both eyes a moment later, file names
// labelled with the task, so each problem is seen in the layer it lives in.
static std::mutex g_uiDumpMx;
struct UiDumpReq { uint64_t due; std::string label; };
static std::vector<UiDumpReq> g_uiDumpQ;
static int g_uiDumpsTaken = 0;
static bool UiDumpWorthy(const std::string& n) {
    std::string l = n; for (auto& c : l) c = (char)tolower((unsigned char)c);
    for (const char* k : { "btl", "battle", "msg", "chat", "camp", "menu", "shop", "panel", "talk", "bustuplip", "general_", "net_scr", "scn_change", "save", "cmm", "event_proc", "evtcutin" }) if (l.find(k) != std::string::npos) return true;
    return false;
}
static void UiDumpSchedule(uint64_t due, const std::string& label) {
    std::lock_guard<std::mutex> lk(g_uiDumpMx);
    if (g_uiDumpsTaken + (int)g_uiDumpQ.size() >= 120) return;
    for (auto& q : g_uiDumpQ) if (q.label == label) return;
    g_uiDumpQ.push_back({ due, label });
}
bool UiDumpTake(uint64_t frame, char* label, size_t cap) {
    std::lock_guard<std::mutex> lk(g_uiDumpMx);
    static uint64_t s_last = 0;
    if (frame < s_last + 8) return false;     // close sequences need dumps a few frames apart
    for (size_t i = 0; i < g_uiDumpQ.size(); ++i) if (g_uiDumpQ[i].due <= frame) {
        strncpy_s(label, cap, g_uiDumpQ[i].label.c_str(), _TRUNCATE);
        for (char* c = label; *c; ++c) if (!isalnum((unsigned char)*c) && *c != '_') *c = '_';
        g_uiDumpQ.erase(g_uiDumpQ.begin() + i); ++g_uiDumpsTaken; s_last = frame;
        return true;
    }
    return false;
}
// [EVTHEAD] 10-09: first person in events (confidant) needs Joker's head INSIDE the event scene.
// OpenGFD layout: Scene +0x10 camera, +0x50 hierarchy root; Node +0x20 world matrix, +0xd0 parent,
// +0xd8 child, +0xe0 next sibling, +0xf0 name. Log every head bone with its ancestor names (Joker's
// model is c0001) and the active camera's world position, bounded.
std::atomic<bool> g_battleRunning{false};
std::atomic<bool> g_eventRunning{false}, g_cmmRunning{false}, g_cutInRunning{false};
// [KNOCKOUT] 10-09: the "remove bars" switch never fired (no [norender] line) and the bars stay. Every
// task alive during an event is offered to the present thread, which switches each one's render
// callback (+0x38) off for 4 frames and measures a bar pixel: the task whose absence clears it draws them.
struct TaskRef { std::string name; uintptr_t task; uintptr_t render; };
static std::mutex g_taskRefMx; static std::vector<TaskRef> g_taskRefs;
// [EVENTFP] 10-09 probe: in the confidant event Joker's model holds c0001_face01/c0001_body/c0001_hand,
// the partner's c0002_...; both skeletons have one "Bip01 Head" (world matrix at node +0x20). Find
// Joker's head and the nearest other head each census; the camera upload reads both matrices live.
std::atomic<uintptr_t> g_evJokerHead{0}, g_evPartnerHead{0};
std::atomic<uintptr_t> g_evScene{0}, g_evSceneRoot{0};
static bool ModelIsJoker(uintptr_t model) {
    static uintptr_t s_m[64]; static int8_t s_v[64]; static int s_n = 0;
    for (int i = 0; i < s_n; ++i) if (s_m[i] == model) return s_v[i] == 1;
    bool joker = false; uintptr_t todo[128]; unsigned pending = 0, visited = 0; todo[pending++] = model;
    while (pending && visited < 400 && !joker) {
        uintptr_t n = todo[--pending]; ++visited; char nm[80] = {};
        if (ProbeName(n + 0xf0, nm) && !strncmp(nm, "c0001_", 6)) joker = true;
        uintptr_t child = 0, next = 0; Rd(n + 0xd8, child); if (n != model) Rd(n + 0xe0, next);
        if (next && next != n && pending < 128) todo[pending++] = next;
        if (child && child != n && pending < 128) todo[pending++] = child;
    }
    if (s_n < 64) { s_m[s_n] = model; s_v[s_n] = joker ? 1 : 0; ++s_n; } else s_n = 0;   // models get recycled: forget all
    return joker;
}
static void FindEventHeads(uintptr_t global, uint64_t frame) {
    uintptr_t joker = 0, jokerScene = 0, jokerRoot = 0; float jp[3] = {}; float jm[16] = {}; uintptr_t others[16]; float op[16][3]; unsigned no = 0;
    for (int si = 0; si < 2; ++si) {
        uintptr_t scene = 0, root = 0;
        if (!Rd(global + 0x60 + si * 8, scene) || !scene || !Rd(scene + 0x50, root) || !root) continue;
        struct It { uintptr_t node, model; };
        It todo[256]; unsigned pending = 0, visited = 0; todo[pending++] = { root, 0 };
        while (pending && visited < 6000) {
            It it = todo[--pending]; ++visited; char name[80] = {};
            const bool named = ProbeName(it.node + 0xf0, name);
            if (named && HeadName(name) && it.model) {
                float m[16]; if (ProbeRead(it.node + 0x20, m, 64)) {
                    if (!joker && ModelIsJoker(it.model)) { joker = it.node; jokerScene = scene; jokerRoot = root; memcpy(jp, m + 12, 12); memcpy(jm, m, 64); }
                    else if (no < 16) { others[no] = it.node; memcpy(op[no], m + 12, 12); ++no; }
                }
            }
            const uintptr_t model = (named && !strcmp(name, "RootNode")) ? it.node : it.model;
            uintptr_t child = 0, next = 0; Rd(it.node + 0xd8, child); Rd(it.node + 0xe0, next);
            if (next && next != it.node && pending < 256) todo[pending++] = { next, it.model };
            // 10-11 log: the "partner" was 30-40 units away = Morgana attached to Joker's own model (bag / desk): skip his subtree
            if (child && child != it.node && pending < 256 && !(named && !strcmp(name, "morugana_root"))) todo[pending++] = { child, model };
        }
    }
    uintptr_t partner = 0; float best = 600.f * 600.f;
    // 10-11: only a person IN FRONT of Joker (within ~50 deg of his facing) at about his head height, 60+ units
    // away (classmates beside him made the view face sideways; two equal ones made it flip). Keep the current
    // partner while it still qualifies.
    float fx = 0, fz = 0; const bool haveFacing = joker && FaceForward(jm, fx, fz);
    auto ok = [&](unsigned i, float& d2) {
        if (fabsf(op[i][1] - jp[1]) > 45.f) return false;
        const float dx = op[i][0] - jp[0], dz = op[i][2] - jp[2]; d2 = dx * dx + dz * dz + (op[i][1] - jp[1]) * (op[i][1] - jp[1]);
        if (d2 < 60.f * 60.f || d2 > 600.f * 600.f) return false;
        if (haveFacing) { const float h = sqrtf(dx * dx + dz * dz); if (h < 1e-3f || (dx * fx + dz * fz) / h < 0.64f) return false; }
        return true; };
    const uintptr_t cur = g_evPartnerHead.load();
    for (unsigned i = 0; i < no; ++i) { float d2; if (others[i] == cur && ok(i, d2)) { partner = cur; best = d2; } }
    if (!partner) for (unsigned i = 0; i < no; ++i) { float d2; if (ok(i, d2) && d2 < best) { best = d2; partner = others[i]; } }
    g_evScene.store(jokerScene); g_evSceneRoot.store(jokerRoot);
    // Publish both even when Joker changed; || used to skip the partner update.
    const bool jokerChanged = g_evJokerHead.exchange(joker) != joker;
    const bool partnerChanged = g_evPartnerHead.exchange(partner) != partner;
    if (jokerChanged || partnerChanged) {
        static int s_logged = 0;
        if (g_log && s_logged++ < 60) g_log("[eventfp] f%llu joker head=%p (%.1f %.1f %.1f) partner head=%p dist=%.1f other heads=%u", (unsigned long long)frame, (void*)joker, jp[0], jp[1], jp[2], (void*)partner, partner ? sqrtf(best) : 0.f, no);
    }
}
// The battle task owns the primary scene, Hinny tha GOAT, throughout intro/turn/attack camera switches.
// It has no field-camera task or event head requirement.
bool BattleSceneWanted() { return g_battleRunning.load() && !g_screenMode.load(); }
bool BattleCullIdentity(uintptr_t& camera, uintptr_t& scene, uintptr_t& root) {
    if(!BattleSceneWanted())return false;
    uint32_t kind=0;float transform[16];
    return Rd(g_global.load()+0x60,scene)&&scene&&Rd(scene+0x10,camera)&&
        Rd(camera,kind)&&kind==5&&Rd(scene+0x50,root)&&NodeMatrix(root,transform);
}
// The event camera may be far from the eye injected at the GPU upload boundary.
// Only the scene containing the validated event Joker belongs to this override.
bool EventFirstPersonWanted() {
    if (!g_eventRunning.load() || g_screenMode.load()) return false;
    const auto s = CameraControls::Get();
    return g_cmmRunning.load() ? s.eventFirstPerson : s.cutsceneFirstPerson;
}
bool EventCullIdentity(uintptr_t& camera, uintptr_t& scene, uintptr_t& root) {
    if (!EventFirstPersonWanted()) return false;
    scene = g_evScene.load(); root = g_evSceneRoot.load();
    uintptr_t s0 = 0, s1 = 0, currentRoot = 0; uint32_t kind = 0;
    if (!scene || !root || !g_evJokerHead.load() ||
        !Rd(g_global.load()+0x60,s0) || !Rd(g_global.load()+0x68,s1) || (scene!=s0 && scene!=s1) ||
        !Rd(scene+0x50,currentRoot) || currentRoot!=root || !Rd(scene+0x10,camera) ||
        !Rd(camera,kind) || kind!=5) return false;
    char name[80] = {}; float m[16];
    return ProbeName(g_evJokerHead.load()+0xf0,name) && HeadName(name) &&
        ProbeRead(g_evJokerHead.load()+0x20,m,sizeof(m)) &&
        std::isfinite(m[12]) && std::isfinite(m[13]) && std::isfinite(m[14]);
}
// [LETTERBOX] 10-09: the event bars are drawn by the "evt_letter_box" task (alive from boot). OpenGFD
// Task: update_fn +0x30, render_fn +0x38. With the option on, its render callback is swapped for a
// no-op (atomic pointer swap, same as the field-camera hook); restored when the option goes off.
static uint64_t NoTaskRender(uintptr_t) { return 0; }
// [LETTERBOXRENDER] 10-12: the update enqueues a separate callback via
// lea rax,[rip+callback]; mov [rdx+0x10],rax. That callback advances the
// animation first, then tests work.flags & 0x400000 before drawing the frame.
// Temporarily set that exact render gate, preserving the event state machine,
// animation and all other UI tasks. Never guess the animation's offsets.
using LbRenderFn = uint64_t(*)(uintptr_t, uintptr_t);
static LbRenderFn g_lbRenderOrig = nullptr;
static std::atomic<uint32_t> g_lbZeroed{0};
static bool LbRenderBit(uintptr_t work, bool hide) {
    __try {
        if (hide) InterlockedOr((volatile LONG*)work, 0x400000);
        else InterlockedAnd((volatile LONG*)work, ~0x400000);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static uint64_t LbRender(uintptr_t queue, uintptr_t work) {
    uint32_t flags = 0;
    const bool mask = CameraControls::Get().removeLetterbox &&
        Rd(work, flags) && !(flags & 0x400000) && LbRenderBit(work, true);
    uint64_t result = 0;
    __try { result = g_lbRenderOrig ? g_lbRenderOrig(queue, work) : 0; }
    __finally {
        if (mask) { LbRenderBit(work, false); g_lbZeroed.fetch_add(1, std::memory_order_relaxed); }
    }
    return result;
}
static void LetterboxHook(uintptr_t task) {
    if (!task || g_lbRenderOrig) return;
    uintptr_t update = 0;
    if (!Rd(task + 0x30, update) || !update) return;
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery((void*)update, &mi, sizeof(mi)) || mi.AllocationBase != GetModuleHandleW(nullptr)) return;
    uint8_t code[0x240] = {};
    if (!SafeRead((void*)update, code, sizeof(code))) return;
    const unsigned char expected[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8d,0x8a,0x98,0,0,0,0x48,0x8b,0xda};
    const unsigned char starts[] = {0,2,6,13};
    const unsigned char gate[] = {0xf7,0x03,0,0,0x40,0,0x0f,0x85};
    for (size_t i = 0; i + 11 <= sizeof(code); ++i) {
        if (memcmp(code + i, "\x48\x8d\x05", 3) || memcmp(code + i + 7, "\x48\x89\x42\x10", 4)) continue;
        int32_t relative = 0; memcpy(&relative, code + i + 3, 4);
        const uintptr_t callback = update + i + 7 + relative;
        uint8_t body[33] = {};
        if (!VirtualQuery((void*)callback, &mi, sizeof(mi)) || mi.AllocationBase != GetModuleHandleW(nullptr) ||
            !SafeRead((void*)callback, body, sizeof(body)) || memcmp(body, expected, sizeof(expected)) ||
            body[16] != 0xe8 || memcmp(body + 21, gate, sizeof(gate))) continue;
        const bool ok = P5RAbsoluteHook::Install((void*)callback, (void*)LbRender, (void**)&g_lbRenderOrig,
            expected, sizeof(expected), starts, sizeof(starts));
        if (g_log) g_log("[letterbox-render] queued callback %p hook=%d: dedicated frame draw gate; event animation preserved", (void*)callback, (int)ok);
        return;
    }
    static bool warned = false;
    if (!warned && g_log) { warned = true; g_log("[letterbox-render] callback signature not recognised; leaving frame unchanged"); }
}
// [NORENDER] one task's render callback switched off / restored on demand (letterbox, event overlay)
struct NoRender { const char* name; uintptr_t task, orig; };
static NoRender g_noRender[2] = { { "evt_letter_box", 0, 0 }, { "EventCtrlPostEffect", 0, 0 } };
static void NoRenderMaintain(NoRender& r, uintptr_t task, bool want) {
    if (!task) return;
    uintptr_t cur = 0; if (!Rd(task + 0x38, cur)) return;
    if (task != r.task) { r.task = task; if (cur != (uintptr_t)NoTaskRender) r.orig = cur; }
    if (want && cur != (uintptr_t)NoTaskRender && cur) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery((void*)cur, &mi, sizeof(mi)) || mi.AllocationBase != GetModuleHandleW(nullptr)) return;
        r.orig = cur;
        if (InterlockedCompareExchangePointer((void* volatile*)(task + 0x38), (void*)NoTaskRender, (void*)cur) == (void*)cur && g_log)
            g_log("[norender] %s render %p -> no-op (task %p)", r.name, (void*)cur, (void*)task);
    } else if (!want && cur == (uintptr_t)NoTaskRender && r.orig) {
        if (InterlockedCompareExchangePointer((void* volatile*)(task + 0x38), (void*)r.orig, (void*)NoTaskRender) == (void*)NoTaskRender && g_log)
            g_log("[norender] %s render restored %p", r.name, (void*)r.orig);
    }
}
// [EVTHEAD] the non-bone node names inside one character model (once per model): finds which is Joker (c0001)
static void ModelNames(uintptr_t model, uint64_t frame) {
    static uintptr_t s_done[32]; static int s_n = 0;
    for (int i = 0; i < s_n; ++i) if (s_done[i] == model) return;
    if (s_n < 32) s_done[s_n++] = model; else return;
    std::string out; uintptr_t todo[256]; unsigned pending = 0, visited = 0; todo[pending++] = model;
    while (pending && visited < 3000 && out.size() < 600) {
        uintptr_t n = todo[--pending]; ++visited; char nm[80] = {};
        if (ProbeName(n + 0xf0, nm) && strncmp(nm, "Bip01", 5) && strncmp(nm, "Bone", 4)) { out += nm; out += " "; }
        uintptr_t child = 0, next = 0; Rd(n + 0xd8, child); if (n != model) Rd(n + 0xe0, next);
        if (next && next != n && pending < 256) todo[pending++] = next;
        if (child && child != n && pending < 256) todo[pending++] = child;
    }
    if (g_log) g_log("[evthead] f%llu model=%p nodes=%u names: %s", (unsigned long long)frame, (void*)model, visited, out.c_str());
}
static void EventHeadProbe(uintptr_t global, uint64_t frame) {
    static int s_runs = 0; if (s_runs >= 40 || !g_log) return; ++s_runs;
    for (int si = 0; si < 2; ++si) {
        uintptr_t scene = 0, root = 0, cam = 0;
        if (!Rd(global + 0x60 + si * 8, scene) || !scene || !Rd(scene + 0x50, root) || !root) continue;
        Rd(scene + 0x10, cam);
        float cm[16] = {};   // camera node? log its object type and, if a node owns it, nothing more here
        // run 10-09: the +0xd0 parent walk gave no names; carry the path of named ancestors down instead
        struct Item { uintptr_t node; std::string path; int named; uintptr_t model; };
        std::vector<Item> todo; todo.reserve(256); todo.push_back({ root, "", 0, 0 }); unsigned visited = 0, heads = 0;
        while (!todo.empty() && visited < 6000) {
            Item it = std::move(todo.back()); todo.pop_back(); ++visited;
            char name[80] = {}; const bool named = ProbeName(it.node + 0xf0, name);
            if (named && HeadName(name) && heads < 12) {
                ++heads; float m[16]; ProbeRead(it.node + 0x20, m, 64);
                g_log("[evthead] f%llu scene%d head=%p pos=(%.1f %.1f %.1f) model=%p path: %s", (unsigned long long)frame, si, (void*)it.node, m[12], m[13], m[14], (void*)it.model, it.path.c_str());
                if (it.model) ModelNames(it.model, frame);
            }
            std::string childPath = it.path; int childNamed = it.named;
            if (named && it.named < 6) { childPath += name; childPath += ">"; ++childNamed; }
            const uintptr_t childModel = (named && !strcmp(name, "RootNode")) ? it.node : it.model;   // each character's own RootNode
            uintptr_t child = 0, next = 0; Rd(it.node + 0xd8, child); Rd(it.node + 0xe0, next);
            if (next && next != it.node && todo.size() < 256) todo.push_back({ next, it.path, it.named, it.model });
            if (child && child != it.node && todo.size() < 256) todo.push_back({ child, childPath, childNamed, childModel });
        }
        uint32_t ctype = 0; if (cam) Rd(cam, ctype);
        g_log("[evthead] f%llu scene%d root=%p nodes visited=%u heads=%u camera=%p type=%u", (unsigned long long)frame, si, (void*)root, visited, heads, (void*)cam, ctype);
        (void)cm;
    }
}
void ScreenTaskCensus(uintptr_t global, uint64_t frame) {
    if (frame % 4) return;
    LoadScreenRules();
    uintptr_t visited[1024]; unsigned count = 0;
    std::vector<std::string> names; names.reserve(160);
    const char* match = nullptr; std::string matchName, flatName; bool eventRunning = false, cmmSeen = false, cutInSeen = false, battleSeen = false; uintptr_t lbTask = 0, peTask = 0; std::vector<TaskRef> taskRefs;
    for (unsigned off = 0x2800; off < 0x2cf0 && count < 1024; off += 8) {
        uintptr_t task = 0; if (!Rd(global + off, task)) continue;
        for (unsigned chain = 0; task > 0x10000 && task < 0x7FFFFFFFFFFF && chain < 512 && count < 1024; ++chain) {
            bool seen = false; for (unsigned i = 0; i < count; ++i) if (visited[i] == task) { seen = true; break; }
            if (seen) break;
            visited[count++] = task;
            uint32_t status = 0; char name[80];
            if (!Rd(task, status) || status < 1 || status > 3 || !TaskNameGuarded(task + 0x10, name)) break;
            names.emplace_back(name);
            {   uintptr_t rf = 0; Rd(task + 0x38, rf); taskRefs.push_back({ name, task, rf }); }
            if (!strcmp(name, "evt_letter_box")) lbTask = task;
            if (!strcmp(name, "EventCtrlPostEffect")) peTask = task;
            // 10-08: CHAT_SYSTEM_LIST lingers ~100 s after a phone chat, through whole events (confidant
            // e707_020 played under it, frozen + its models removed as "menu-only"): never a screen task.
            if (!match && g_screenRules.enabled && IsScreenTask(name) && strcmp(name, "CHAT_SYSTEM_LIST")) { matchName = name; match = matchName.c_str(); }
            if (!strcmp(name, "event_proc")) eventRunning = true;
            if (!strcmp(name, "battle")) battleSeen = true;
            if (!strncmp(name, "cmm", 3)) cmmSeen = true;
            if (!strcmp(name, "evtCutIn")) cutInSeen = true;   // 10-09: Joker's choice cut-in   // confidant tasks (cmmActive, cmmEvent, cmm_pc_icon...)
            if (flatName.empty() && g_screenRules.enabled && (IsFlatTask(name) || IsSolidTask(name))) flatName = name;
            uintptr_t next = 0; if (!Rd(task + 0x80, next) || next == task) break;
            task = next;
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    if (g_log && !names.empty()) {
        unsigned logged = 0;
        for (auto& n : names) if (!std::binary_search(g_taskNames.begin(), g_taskNames.end(), n)) {
            if (logged++ < 32) g_log("[tasks] f%llu + %s", (unsigned long long)frame, n.c_str());
            if (UiDumpWorthy(n)) UiDumpSchedule(frame + 90, n + "_on");          // ~1.5 s in: settled
        }
        for (auto& n : g_taskNames) if (!std::binary_search(names.begin(), names.end(), n)) {
            if (logged++ < 32) g_log("[tasks] f%llu - %s", (unsigned long long)frame, n.c_str());
            if (UiDumpWorthy(n)) UiDumpSchedule(frame + 240, n + "_after");      // ~4 s after it ended: what stays behind
        }
    }
    if (!names.empty()) g_taskNames.swap(names);
    g_battleRunning.store(battleSeen);
    g_eventRunning.store(eventRunning, std::memory_order_relaxed);
    g_cmmRunning.store(eventRunning && cmmSeen, std::memory_order_relaxed);
    g_cutInRunning.store(eventRunning && cutInSeen, std::memory_order_relaxed);
    { std::lock_guard<std::mutex> lk(g_taskRefMx); g_taskRefs.swap(taskRefs); }
    {   // [LBWORK] 10-09: evt_letter_box has no render callback (render=null): the bars come from its
        // update. Read-only: log its work block (+0x48) whenever it changes (event start/end, bars in/out),
        // to find the field that sizes or shows the bars.
        static uint8_t s_prev[0x100]; static bool s_have = false; static int s_logs = 0; static uint64_t s_last = 0; static bool s_evWas = false;
        uintptr_t work = 0; uint8_t cur[0x100];
        if (lbTask && s_logs < 60 && g_log && (frame >= s_last + 60 || eventRunning != s_evWas) && Rd(lbTask + 0x48, work) && work && ProbeRead(work, cur, sizeof(cur)) && (!s_have || memcmp(cur, s_prev, sizeof(cur)))) {
            ++s_logs; s_last = frame; s_evWas = eventRunning; std::string line;
            for (int i = 0; i < 0x100; i += 4) {
                uint32_t u; float fl; memcpy(&u, cur + i, 4); memcpy(&fl, cur + i, 4);
                uint32_t pu = 0; memcpy(&pu, s_prev + i, 4);
                char b[48]; const bool changed = s_have && u != pu;
                if (u == 0 && !changed) continue;
                if (std::isfinite(fl) && fabsf(fl) > 1e-4f && fabsf(fl) < 1e5f) snprintf(b, sizeof(b), "%s+%02X=%.3f ", changed ? "*" : "", i, fl);
                else snprintf(b, sizeof(b), "%s+%02X=%08X ", changed ? "*" : "", i, u);
                line += b;
            }
            g_log("[lbwork] f%llu event=%d work=%p %s", (unsigned long long)frame, (int)eventRunning, (void*)work, line.c_str());
            memcpy(s_prev, cur, sizeof(cur)); s_have = true;
        }
    }
    {   static bool s_lbLogged = false;   // why the bars switch never fired: the task's callback slots
        if (lbTask && !s_lbLogged && g_log) { s_lbLogged = true; uintptr_t u = 0, r = 0, e = 0; Rd(lbTask + 0x30, u); Rd(lbTask + 0x38, r); Rd(lbTask + 0x40, e);
            g_log("[norender] evt_letter_box task=%p update=%p render=%p end=%p exe=%p", (void*)lbTask, (void*)u, (void*)r, (void*)e, (void*)GetModuleHandleW(nullptr)); } }
    LetterboxHook(lbTask);
    if (frame % 600 == 0 && g_lbZeroed.load() && g_log) g_log("[letterbox] f%llu dedicated frame draws masked in 600f=%u", (unsigned long long)frame, g_lbZeroed.exchange(0));
    NoRenderMaintain(g_noRender[1], peTask, CameraControls::Get().removeEventOverlay);   // 10-09: starry layer over confidant scenes (candidate)
    if (eventRunning && (CameraControls::Get().eventFirstPerson || CameraControls::Get().cutsceneFirstPerson)) FindEventHeads(global, frame);
    else { g_evJokerHead.store(0); g_evPartnerHead.store(0); g_evScene.store(0); g_evSceneRoot.store(0); }
    if (eventRunning && frame % 600 == 0) { char l[48]; snprintf(l, sizeof(l), "event_f%llu", (unsigned long long)frame); UiDumpSchedule(frame, l); }   // [EVTDUMP] what is on screen during events (confidant overlay)
    if (eventRunning && frame % 240 == 0) EventHeadProbe(global, frame);
    {   // [CAMPROBE] 10-08: goal is first person in dialogue cutscenes, reading, confidants... Log every
        // change of the scene's active camera: field camera or not, its position, and the event/talk tasks
        // running then - the map of which camera owns each kind of scene.
        uintptr_t active = 0;
        for (int i = 0; i < 2 && !active; ++i) { uintptr_t scene = 0; if (Rd(global + 0x60 + i * 8, scene) && scene) Rd(scene + 0x10, active); }
        static uintptr_t s_lastCam = 0; static int s_logged = 0;
        if (active != s_lastCam && s_logged < 400) {
            ++s_logged; s_lastCam = active;
            uintptr_t fcam = 0, froot = 0; const bool isField = active && FieldIdentity(g_contextTask.load(), fcam, froot, false) && fcam == active;
            float v[16] = {}; float pos[3] = {};
            if (active && ProbeRead(active + 0x20, v, 64)) for (int j = 0; j < 3; ++j) pos[j] = -(v[j*4]*v[3] + v[j*4+1]*v[7] + v[j*4+2]*v[11]);   // guess: row-major view at +0x20
            std::string ev;
            for (auto& n : g_taskNames) {
                std::string l = n; for (auto& c : l) c = (char)tolower((unsigned char)c);
                const bool evt = l.find("event") != std::string::npos || l.find("evt") != std::string::npos || l.find("talk") != std::string::npos
                    || l.find("bustup") != std::string::npos || l.find("cmm") != std::string::npos || l.find("main_") == 0 || l.find("msg") != std::string::npos
                    || (l.size() > 4 && l[0] == 'e' && isdigit((unsigned char)l[1]) && isdigit((unsigned char)l[2]) && isdigit((unsigned char)l[3]));
                if (evt && ev.size() < 400) { ev += n; ev += " | "; }
            }
            if (g_log) g_log("[camprobe] f%llu active camera %p field=%d pos=(%.1f %.1f %.1f) tasks: %s", (unsigned long long)frame, (void*)active, (int)isField, pos[0], pos[1], pos[2], ev.c_str());
        }
    }
    // [RAILMAP] 10-08: the rail map (Shibuya / Yongen-Jaya...) = CMP_DRAW_MONEY_PANEL inside fld_r1_menu
    // (both logs: present exactly while it is up). Panel: red frame, no lines. Shown as the flat screen.
    if (std::binary_search(g_taskNames.begin(), g_taskNames.end(), std::string("fld_r1_menu"))
        && std::binary_search(g_taskNames.begin(), g_taskNames.end(), std::string("CMP_DRAW_MONEY_PANEL"))) flatName = "rail_map";
    if (eventRunning) match = nullptr;   // [EVENTS] a running event (cutscene, confidant, talk scene) is never a menu
    // 10-08 log: fld_scn_change stays alive through whole events (e881_201_MoruTalk, ...): not a loading screen then
    if (eventRunning && flatName == "fld_scn_change") flatName.clear();
    if (!flatName.empty()) { matchName = flatName; match = matchName.c_str(); }   // [FLATSCREEN]/[SOLIDPANEL] take priority
    // Enter at once; leave only after three empty censuses (12 frames) so menu fades don't flicker.
    static int s_absent = 0;
    if (match) {
        s_absent = 0;
        if (!g_screenMode.load(std::memory_order_relaxed) || strcmp(g_screenTask, match) != 0) {
            if (!g_screenMode.load(std::memory_order_relaxed) && g_log) g_log("[screen] ON f%llu task=%s", (unsigned long long)frame, match);
            strncpy_s(g_screenTask, match, _TRUNCATE);
            g_screenMode.store(true, std::memory_order_relaxed);
        }
    } else if (g_screenMode.load(std::memory_order_relaxed) && ++s_absent >= 3) {
        if (g_log) g_log("[screen] OFF f%llu (was %s)", (unsigned long long)frame, g_screenTask);
        g_screenMode.store(false, std::memory_order_relaxed);
        g_screenTask[0] = 0;
    }
}
