/* mspptool.exe - MSCC data viewer / stepper (MS Ponder Puzzlers, dirty-side tool).
 *
 * Runs CHIPS.EXE from a working copy whose game timer is switched off (two 2-byte timer intervals set
 * to 0xFFFF), so this tool supplies every game tick: play, pause, single step, speed. DATATAP.EXE
 * (Win16 helper, same 16-bit session as the game) lists the game's data blocks; this tool reads and
 * writes only those blocks (code is never listed) with ReadProcessMemory / WriteProcessMemory.
 *
 * Works on Windows (16-bit support through NTVDM or otvdm; set the launcher in Setup) and on Linux under Wine.
 * Build: i686-w64-mingw32-gcc -O2 -mwindows -o mspptool.exe mspptool.c -lcomdlg32 -lgdi32
 * Test mode (no GUI): mspptool.exe /test GAMEDIR DATFILE INPUTFILE OUTFILE
 */
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ settings and paths */
static char toolDir[MAX_PATH], iniPath[MAX_PATH], gameDir[MAX_PATH], datPath[MAX_PATH], workDir[MAX_PATH], vdmCmd[MAX_PATH], inputPath[MAX_PATH];
static int testMode;
static FILE *testOut;

static void status(const char *fmt, ...);
static void save_settings(void) {
    WritePrivateProfileStringA("mspptool", "GameDir", gameDir, iniPath);
    WritePrivateProfileStringA("mspptool", "LevelSet", datPath, iniPath);
    WritePrivateProfileStringA("mspptool", "Launcher", vdmCmd, iniPath);
}
static void load_settings(void) {
    GetPrivateProfileStringA("mspptool", "GameDir", "", gameDir, MAX_PATH, iniPath);
    GetPrivateProfileStringA("mspptool", "LevelSet", "", datPath, MAX_PATH, iniPath);
    GetPrivateProfileStringA("mspptool", "Launcher", "", vdmCmd, MAX_PATH, iniPath);
}

/* ------------------------------------------------------------------ level set (DAT) */
#define MAXLEV 4000
static int nLevels;
static char levPw[MAXLEV + 1][10], levTitle[MAXLEV + 1][64];

static int read_dat(const char *path) {
    FILE *f = fopen(path, "rb"); unsigned char h[6]; nLevels = 0;
    if (!f) return 0;
    if (fread(h, 1, 6, f) != 6) { fclose(f); return 0; }
    int count = h[4] | h[5] << 8;
    for (int i = 0; i < count && i < MAXLEV; i++) {
        unsigned char sz[2]; if (fread(sz, 1, 2, f) != 2) break;
        int n = sz[0] | sz[1] << 8; unsigned char *r = malloc(n);
        if (!r || (int)fread(r, 1, n, f) != n) { free(r); break; }
        int num = r[0] | r[1] << 8, p = 8, l1 = r[p] | r[p + 1] << 8; p += 2 + l1;
        int l2 = r[p] | r[p + 1] << 8; p += 2 + l2; int ol = r[p] | r[p + 1] << 8; p += 2;
        int end = p + ol; if (end > n) end = n;
        if (num < 1 || num > MAXLEV) num = i + 1;
        levPw[num][0] = 0; levTitle[num][0] = 0;
        while (p + 2 <= end) {
            int t = r[p], len = r[p + 1]; p += 2;
            if (t == 6) { int k; for (k = 0; k < len && k < 9 && r[p + k]; k++) levPw[num][k] = r[p + k] ^ 0x99; levPw[num][k] = 0; }
            if (t == 3) { int k; for (k = 0; k < len && k < 63 && r[p + k]; k++) levTitle[num][k] = r[p + k]; levTitle[num][k] = 0; }
            p += len;
        }
        if (num > nLevels) nLevels = num;
        free(r);
    }
    fclose(f); return nLevels;
}

/* ------------------------------------------------------------------ save file (entpack.ini): back up, rewrite, restore */
static char *iniBackup; static int iniBackedUp;
static void ini_backup(void) {
    if (iniBackedUp) return;
    iniBackup = malloc(65536); iniBackup[0] = iniBackup[1] = 0;
    GetPrivateProfileSectionA("Chip's Challenge", iniBackup, 65536, "entpack.ini");
    iniBackedUp = 1;
}
static void ini_restore(void) {
    if (!iniBackedUp) return;
    WritePrivateProfileSectionA("Chip's Challenge", iniBackup, "entpack.ini");
    WritePrivateProfileStringA(NULL, NULL, NULL, "entpack.ini");   /* flush */
}
static void ini_write_level(int level) {
    char buf[32], key[32];
    ini_backup();
    WritePrivateProfileSectionA("Chip's Challenge", "", "entpack.ini");
    WritePrivateProfileStringA("Chip's Challenge", "Sounds", "0", "entpack.ini");
    WritePrivateProfileStringA("Chip's Challenge", "Number of Midi Files", "0", "entpack.ini");
    sprintf(buf, "%d", level); WritePrivateProfileStringA("Chip's Challenge", "Current Level", buf, "entpack.ini");
    sprintf(buf, "%d", nLevels); WritePrivateProfileStringA("Chip's Challenge", "Highest Level", buf, "entpack.ini");
    for (int i = 1; i <= nLevels; i++) if (levPw[i][0]) { sprintf(key, "Level%d", i); WritePrivateProfileStringA("Chip's Challenge", key, levPw[i], "entpack.ini"); }
    WritePrivateProfileStringA(NULL, NULL, NULL, "entpack.ini");
}

/* ------------------------------------------------------------------ working copy */
static int copy_file(const char *a, const char *b) { return CopyFileA(a, b, FALSE); }
static int noFocusPause;      /* the run copy never pauses on focus loss (then no un-pausing is needed) */
static int setup_work(char *err) {
    char p[MAX_PATH], q[MAX_PATH]; WIN32_FIND_DATAA fd; HANDLE h;
    sprintf(p, "%s\\CHIPS.EXE", gameDir);
    FILE *f = fopen(p, "rb"); if (!f) { sprintf(err, "No CHIPS.EXE in %s", gameDir); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *exe = malloc(n); fread(exe, 1, n, f); fclose(f);
    if (n < 0x2D24 || exe[0x2D1A] != 0xBE || exe[0x2D1D - 1] != 0x00 || exe[0x2D20] != 0xBE ||
        !((exe[0x2D1B] == 0x6E && exe[0x2D1C] == 0x00) || (exe[0x2D1B] == 0xFF && exe[0x2D1C] == 0xFF)) ||
        !((exe[0x2D21] == 0xDC && exe[0x2D22] == 0x00) || (exe[0x2D21] == 0xFF && exe[0x2D22] == 0xFF))) {
        sprintf(err, "This CHIPS.EXE is not the version this tool knows (timer bytes not found at 0x2D1A / 0x2D20)."); free(exe); return 0;
    }
    sprintf(workDir, "%s\\run", toolDir); CreateDirectoryA(workDir, NULL);
    sprintf(p, "%s\\*", gameDir); h = FindFirstFileA(p, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(datPath[0] && !_stricmp(fd.cFileName, "CHIPS.DAT"))) { sprintf(p, "%s\\%s", gameDir, fd.cFileName); sprintf(q, "%s\\%s", workDir, fd.cFileName); copy_file(p, q); } }
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    exe[0x2D1B] = exe[0x2D1C] = exe[0x2D21] = exe[0x2D22] = 0xFF;      /* game timer off: this tool sends the ticks */
    /* ...and never started at all: jump over the SetTimer call to the success path (the 16-bit interval above can
       only slow the game's own timer to 65 s, and a focus change restarts it, so a long replay could get an extra tick) */
    if (exe[0x2D27] == 0x51 && exe[0x2D28] == 0xFF) { exe[0x2D27] = 0xEB; exe[0x2D28] = 0x39; }
    /* no pause when the game loses focus or is minimized (no PAUSED screen; clicking in this tool cannot drop a tick).
       Only the two window-message handlers are changed; the pause routine itself, Game > Pause (F3) and the pauses
       around the game's own dialogs are untouched, and nothing in the game rules calls these handlers.
       0x3AE5  WM_ACTIVATEAPP: jz -> jmp past the pause / un-pause branch
       0x3CEC  WM_SYSCOMMAND minimize / 0x3CF1, 0x3CF6 restore: jump to the handler's common continuation (0x3D2E) */
    if (exe[0x3AE5] == 0x74 && exe[0x3AE6] == 0x27) exe[0x3AE5] = 0xEB;
    if (exe[0x3CEC] == 0x74 && exe[0x3CED] == 0x0C) exe[0x3CED] = 0x40;
    if (exe[0x3CF1] == 0x74 && exe[0x3CF2] == 0x13) exe[0x3CF2] = 0x3B;
    if (exe[0x3CF6] == 0x74 && exe[0x3CF7] == 0x0E) exe[0x3CF7] = 0x36;
    noFocusPause = exe[0x3AE5] == 0xEB && exe[0x3CED] == 0x40;
    sprintf(p, "%s\\CHIPS.EXE", workDir); f = fopen(p, "wb");
    if (!f) { sprintf(err, "Cannot write %s", p); free(exe); return 0; }
    fwrite(exe, 1, n, f); fclose(f); free(exe);
    sprintf(p, "%s\\DATATAP.EXE", toolDir); sprintf(q, "%s\\DATATAP.EXE", workDir);
    if (!copy_file(p, q)) { sprintf(err, "DATATAP.EXE missing next to mspptool.exe"); return 0; }
    if (datPath[0]) { sprintf(q, "%s\\CHIPS.DAT", workDir); if (!copy_file(datPath, q)) { sprintf(err, "Cannot copy level set %s", datPath); return 0; } }
    sprintf(q, "%s\\CHIPS.DAT", workDir);
    if (!read_dat(q)) { sprintf(err, "Cannot read the level set %s", q); return 0; }
    return 1;
}

/* ------------------------------------------------------------------ the running game */
static HWND gGame, gBoard; static DWORD gPid; static HANDLE gProc;
static PROCESS_INFORMATION gPI;
typedef struct { unsigned handle, type, data; unsigned long size, addr; } Block;
static Block blocks[1024]; static int nBlocks, dsIdx = -1;
static unsigned taskInst, stackTop, stackBot, B;

static HWND oldGames[64]; static int nOldGames;     /* MSCC windows that were already open before this launch (not ours) */
static BOOL CALLBACK find_old_games(HWND h, LPARAM lp) {
    char t[256]; GetWindowTextA(h, t, sizeof t);
    if (!strncmp(t, "Chip's Challenge:", 17) && nOldGames < 64) oldGames[nOldGames++] = h; return TRUE;
}
static BOOL CALLBACK find_game(HWND h, LPARAM lp) {
    char t[256]; GetWindowTextA(h, t, sizeof t);
    if (!strncmp(t, "Chip's Challenge:", 17)) { for (int i = 0; i < nOldGames; i++) if (oldGames[i] == h) return TRUE; gGame = h; return FALSE; } return TRUE;
}
static BOOL CALLBACK find_board(HWND h, LPARAM lp) {
    char c[64]; GetClassNameA(h, c, sizeof c);
    if (!strcmp(c, "BoardClass")) { gBoard = h; return FALSE; } return TRUE;
}
static void game_kill(void) {
    if (gProc) { TerminateProcess(gProc, 0); WaitForSingleObject(gProc, 3000); CloseHandle(gProc); gProc = NULL; }
    if (gPI.hProcess) { TerminateProcess(gPI.hProcess, 0); CloseHandle(gPI.hProcess); CloseHandle(gPI.hThread); memset(&gPI, 0, sizeof gPI); }
    HWND t; for (int i = 0; i < 50 && ((t = FindWindowA(NULL, "DataTap")) != NULL); i++) {
        DWORD pid; GetWindowThreadProcessId(t, &pid); HANDLE ph = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (ph) { TerminateProcess(ph, 0); CloseHandle(ph); } Sleep(100);
    }
    gGame = gBoard = NULL; gPid = 0; nBlocks = 0; dsIdx = -1;
}
static int tap_map(void) {
    char p[MAX_PATH], line[512]; HWND t = FindWindowA(NULL, "DataTap"); DWORD_PTR rv;
    if (!t) return 0;
    sprintf(p, "%s\\TAPMAP.TXT", workDir); DeleteFileA(p);
    if (!SendMessageTimeoutA(t, WM_USER + 1, 0, 0, SMTO_NORMAL, 2000, &rv)) return 0;
    FILE *f = NULL; for (int i = 0; i < 200 && !(f = fopen(p, "r")); i++) Sleep(10);
    if (!f) return 0;
    static Block nb[1024]; unsigned ti = taskInst, st2 = stackTop, sb = stackBot;
    int n = 0, ok = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned a, b2, c, d, e, g, hh, k; unsigned long sz, ad;
        if (sscanf(line, "task %x module %x inst %x ss %x sp %x stacktop %x stackmin %x stackbot %x", &a, &b2, &c, &d, &e, &g, &hh, &k) == 8) { ti = c; st2 = g; sb = k; }
        else if (sscanf(line, "block %x owner %x type %u data %u size %lu addr %lx", &a, &b2, &c, &d, &sz, &ad) == 6 && n < 1024) {
            nb[n].handle = a; nb[n].type = c; nb[n].data = d; nb[n].size = sz; nb[n].addr = ad; n++;
        } else if (!strncmp(line, "end", 3)) ok = 1;
    }
    fclose(f);
    int ds = -1; for (int i = 0; i < n; i++) if ((nb[i].handle | 7) == (ti | 7)) ds = i;
    if (!ok || ds < 0) return 0;                 /* keep the previous map */
    memcpy(blocks, nb, n * sizeof(Block)); nBlocks = n; dsIdx = ds; taskInst = ti; stackTop = st2; stackBot = sb;
    return 1;
}
static int blk_of(unsigned handle) {          /* handle 0xFFFF = the game's data segment */
    if (handle == 0xFFFF) return dsIdx;
    for (int i = 0; i < nBlocks; i++) if ((blocks[i].handle | 7) == (handle | 7)) return i;
    return -1;
}
static int mem_read(unsigned handle, unsigned long off, void *buf, unsigned long n) {
    int i = blk_of(handle); SIZE_T got = 0;
    if (i < 0 || off + n > blocks[i].size || !gProc) { memset(buf, 0, n); return 0; }
    if (!ReadProcessMemory(gProc, (LPCVOID)(ULONG_PTR)(blocks[i].addr + off), buf, n, &got) || got != n) { memset(buf, 0, n); return 0; }
    return 1;
}
static int mem_write(unsigned handle, unsigned long off, const void *buf, unsigned long n, char *err) {
    int i = blk_of(handle); SIZE_T put = 0;
    if (i < 0 || off + n > blocks[i].size || !gProc) { if (err) strcpy(err, "outside the block"); return 0; }
    if (i == dsIdx && off < stackBot && off + n > stackTop) { if (err) strcpy(err, "refused: that range is the game's stack"); return 0; }
    if (!WriteProcessMemory(gProc, (LPVOID)(ULONG_PTR)(blocks[i].addr + off), buf, n, &put) || put != n) { if (err) strcpy(err, "write failed"); return 0; }
    return 1;
}
static unsigned rd16(unsigned handle, unsigned long off) { unsigned char b[2]; mem_read(handle, off, b, 2); return b[0] | b[1] << 8; }
static void remap(void) { if (tap_map()) { B = rd16(0xFFFF, 0x1680); if (!B) B = 0x9114; } }

static int game_launch(int level, char *err) {
    char cmd[MAX_PATH * 2];
    game_kill();
    ini_write_level(level);
    if (vdmCmd[0]) sprintf(cmd, "\"%s\" DATATAP.EXE CHIPS.EXE", vdmCmd); else strcpy(cmd, "DATATAP.EXE CHIPS.EXE");
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    nOldGames = 0; EnumWindows(find_old_games, 0);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, workDir, &si, &gPI)) {
        sprintf(err, "Could not start the game (%s). On 64-bit Windows set the 16-bit launcher (otvdm.exe) in Setup.", cmd); return 0;
    }
    for (int i = 0; i < 200 && !gGame; i++) { EnumWindows(find_game, 0); if (!gGame) Sleep(75); }
    if (!gGame) { sprintf(err, "The game window did not appear"); game_kill(); return 0; }
    gBoard = NULL; EnumChildWindows(gGame, find_board, 0);
    GetWindowThreadProcessId(gGame, &gPid);
    gProc = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE, FALSE, gPid);
    if (!gProc) { sprintf(err, "Cannot open the game process for reading"); return 0; }
    Sleep(300);
    for (int i = 0; i < 40; i++) { if (tap_map()) break; Sleep(250); }
    if (dsIdx < 0) { sprintf(err, "DATATAP did not answer (is DATATAP.EXE next to the tool?)"); return 0; }
    remap();
    return 1;
}

/* ------------------------------------------------------------------ game control */
static int tickNo;            /* ticks sent since the level was (re)started by this tool */
static int vk_of(char d) { return d == 'N' ? VK_UP : d == 'S' ? VK_DOWN : d == 'W' ? VK_LEFT : VK_RIGHT; }
static void game_wake(void);
static void key_sync(int vk) {      /* handled by the game's window procedure before this returns */
    game_wake(); UINT sc = MapVirtualKeyA(vk, 0); DWORD_PTR rv;
    SendMessageTimeoutA(gGame, WM_KEYDOWN, vk, 1 | (sc << 16) | (1 << 24), SMTO_NORMAL, 2000, &rv);
    SendMessageTimeoutA(gGame, WM_KEYUP, vk, 1 | (sc << 16) | (1 << 24) | (3u << 30), SMTO_NORMAL, 2000, &rv);
}
static void key_post(int vk) {
    UINT sc = MapVirtualKeyA(vk, 0);
    PostMessageA(gGame, WM_KEYDOWN, vk, 1 | (sc << 16) | (1 << 24));
    PostMessageA(gGame, WM_KEYUP, vk, 1 | (sc << 16) | (1 << 24) | (3u << 30));
    Sleep(10);
}
static void game_start(void) { game_wake(); key_post(VK_SHIFT); }    /* a non-move key starts the level clock */
static int board_valid(void);
static void game_wake(void) {     /* MSCC pauses itself when it loses focus (and then ignores keys and ticks): tell it it is active */
    if (noFocusPause) return;           /* DS:0x24 then only counts message boxes: leave it alone */
    unsigned char paused = 0; DWORD_PTR rv;       /* DS:0x24 = 1 while the game is paused because it lost focus */
    if (gGame && board_valid() && mem_read(0xFFFF, 0x24, &paused, 1) && paused == 1) SendMessageTimeoutA(gGame, WM_ACTIVATEAPP, TRUE, 0, SMTO_NORMAL, 2000, &rv);
}
static int game_tick(void) {
    DWORD_PTR rv; if (!gGame) return 0;
    int ok = 0;
    for (int tries = 0; tries < 5; tries++) {
        game_wake(); unsigned before = rd16(0xFFFF, 0x064E);
        ok = SendMessageTimeoutA(gBoard ? gBoard : gGame, WM_TIMER, 1, 0, SMTO_NORMAL, 2000, &rv) != 0;
        if (!ok) break;
        /* the game may have paused itself (focus moved, e.g. a click in this window) just before the tick: then it ignored
           the tick and its tick counter did not move. Un-pause and send the same tick again. */
        unsigned char paused = 0; mem_read(0xFFFF, 0x24, &paused, 1);
        if (noFocusPause || !(paused == 1 && rd16(0xFFFF, 0x064E) == before && board_valid())) break;
    }
    if (ok) tickNo++;
    remap(); return ok;
}
static void game_click(int dx, int dy) {    /* a click on the square at offset (dx,dy) from Chip, through the 9x9 view */
    int cx = rd16(0xFFFF, B + 0x808), cy = rd16(0xFFFF, B + 0x80A);
    int ox = cx - 4 < 0 ? 0 : cx - 4 > 23 ? 23 : cx - 4, oy = cy - 4 < 0 ? 0 : cy - 4 > 23 ? 23 : cy - 4;
    int px = (cx + dx - ox) * 32 + 16, py = (cy + dy - oy) * 32 + 16; HWND t = gBoard ? gBoard : gGame;
    game_wake();
    PostMessageA(t, WM_MOUSEMOVE, 0, MAKELPARAM(px, py)); PostMessageA(t, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px, py));
    PostMessageA(t, WM_LBUTTONUP, 0, MAKELPARAM(px, py)); Sleep(10);
}
static char dlgNames[4096]; static HWND dlgBox;
static BOOL CALLBACK list_names(HWND h, LPARAM lp) {
    char t[256]; DWORD pid = 0; if (!IsWindowVisible(h)) return TRUE; GetWindowThreadProcessId(h, &pid); if (gPid && pid != gPid) return TRUE; GetWindowTextA(h, t, sizeof t);
    if (!strcmp(t, "Chip's Challenge") || !strcmp(t, "Level Complete!")) dlgBox = h;
    if (t[0] && strlen(dlgNames) + strlen(t) + 2 < sizeof dlgNames) { strcat(dlgNames, t); strcat(dlgNames, "|"); }
    return TRUE;
}
static int board_valid(void) {              /* the level structure is live (it can be destroyed on purpose: rule S80) */
    if (B < 0x100 || B > 0x10000 - 0xB00) return 0;
    unsigned lv = rd16(0xFFFF, B + 0x800), cnt = rd16(0xFFFF, B + 0x802);
    return cnt >= 1 && cnt <= 30000 && lv >= 1 && lv <= cnt;
}
static const char *game_status(void) {      /* "won", "dead" (or another message box), "level data lost", "playing" */
    dlgNames[0] = 0; dlgBox = NULL; EnumWindows(list_names, 0);
    if (gProc && !board_valid()) return "level data lost";
    if (strstr(dlgNames, "Level Complete!")) return "won";
    if (!strncmp(dlgNames, "Chip's Challenge|", 17) || strstr(dlgNames, "|Chip's Challenge|")) return "dead";
    return "playing";
}
static int endedBox; static void level_restarted(void);
static int endWonLevel;
static void game_ok(void) { const char *gs = game_status();
    if (dlgBox && strcmp(gs, "playing")) { endedBox = 1; if (!strcmp(gs, "won")) endWonLevel = rd16(0xFFFF, B + 0x800); }
    if (dlgBox) {           /* press the box's own button: OK on a death box, "Onward!" (id 106) on the win box */
        HWND btn = FindWindowExA(dlgBox, NULL, "Button", NULL); int bid = btn ? GetDlgCtrlID(btn) : IDOK;
        PostMessageA(dlgBox, WM_COMMAND, MAKEWPARAM(bid, BN_CLICKED), (LPARAM)btn);
        for (int i = 0; i < 40; i++) { Sleep(50); remap(); if (!strcmp(game_status(), "playing")) break; }
        if (!strcmp(game_status(), "playing") && endedBox) level_restarted(); } else Sleep(50); }
static void game_restart(void) {
    int id = gGame ? 113 : 0;       /* Level > Restart (menu ids from CHIPS.EXE's menu resource; another process cannot read a 16-bit menu) */
    if (id) { DWORD_PTR rv; SendMessageTimeoutA(gGame, WM_COMMAND, id, 0, SMTO_NORMAL, 2000, &rv); }
    tickNo = 0; Sleep(50); remap();
}

/* ------------------------------------------------------------------ input files */
#define MAXEV 100000
typedef struct { int n; char k[16]; } Ev;
static Ev ev[MAXEV]; static int nEv, inLevel, inTicks, inHasSeed;
static unsigned long inSeed; static int scriptOn, scriptPos, inShiftAlways = 1, wantLevel = 1;
static char inKind[16] = "";
static unsigned char twsHas[MAXLEV + 1]; static int twsCount;    /* levels that have a solution in the loaded .tws */
static int load_input(const char *path, char *err) {
    FILE *f = fopen(path, "rb"); if (!f) { sprintf(err, "Cannot open %s", path); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *s = malloc(n + 1); fread(s, 1, n, f); s[n] = 0; fclose(f);
    nEv = 0; inLevel = 0; inTicks = 0; inSeed = 0; inHasSeed = 0; inShiftAlways = 1; strcpy(inKind, "text");
    if (n >= 8 && (unsigned char)s[0] == 0x35 && (unsigned char)s[1] == 0x33 && (unsigned char)s[2] == 0x9B && (unsigned char)s[3] == 0x99) {
        /* Tile World solution file: take the level in the Level box. Times are Tile World ticks (2 per game tick). */
        unsigned char *d = (unsigned char *)s; long off = 8 + d[7]; int found = 0; strcpy(inKind, "TWS"); inShiftAlways = 0;
        memset(twsHas, 0, sizeof twsHas); twsCount = 0;
        static const char *IDX = "NWSE";
        while (off + 4 <= n) {
            unsigned long size = d[off] | d[off + 1] << 8 | d[off + 2] << 16 | (unsigned long)d[off + 3] << 24; off += 4;
            if (size == 0xFFFFFFFFul) break; if (!size) continue; if (off + (long)size > n) break;
            unsigned char *r = d + off; off += size; int num = r[0] | r[1] << 8;
            if (size > 16 && num > 0 && num <= MAXLEV && !twsHas[num]) { twsHas[num] = 1; twsCount++; }
            if (num != wantLevel || size <= 16 || found) continue;
            found = 1; inLevel = num; unsigned long best = r[12] | r[13] << 8 | r[14] << 16 | (unsigned long)r[15] << 24;
            {   /* MSCC solution pack (.mscc.tws): bytes 8-11 hold MSCC's own random state at level start */
                size_t pl = strlen(path);
                if (pl > 9 && !_stricmp(path + pl - 9, ".mscc.tws")) { inSeed = r[8] | r[9] << 8 | r[10] << 16 | (unsigned long)r[11] << 24; inHasSeed = 1; strcpy(inKind, "MSCC TWS"); }
            }
            unsigned char *p = r + 16; long m = size - 16, i = 0; long when = -1;
            #define ADD(w, str) do { if (nEv < MAXEV) { ev[nEv].n = (int)((w) / 2); strcpy(ev[nEv].k, str); nEv++; } } while (0)
            while (i < m) {
                int b = p[i], k = b & 3; char one[2] = { 0, 0 };
                if (k == 0) { for (int sh = 2; sh <= 6; sh += 2) { when += 4; one[0] = IDX[(b >> sh) & 3]; ADD(when, one); } i += 1; }
                else if (k == 1) { when += ((b >> 5) & 7) + 1; int dv = (b >> 2) & 7; if (dv < 4) { one[0] = IDX[dv]; ADD(when, one); } i += 1; }
                else if (k == 2) { if (i + 1 >= m) break; when += ((b >> 5) & 7) + (p[i + 1] << 3) + 1; int dv = (b >> 2) & 7; if (dv < 4) { one[0] = IDX[dv]; ADD(when, one); } i += 2; }
                else if (b & 0x10) {
                    int mm = (b >> 2) & 3; if (i + 2 + mm > m) break;
                    int dirv = ((b >> 5) & 7) | ((p[i + 1] & 0x3F) << 3); when += (p[i + 1] >> 6) & 3;
                    for (int j = mm - 1; j >= 0; j--) when += (long)p[i + 2 + j] << (2 + j * 8);
                    when += 1;
                    if (dirv >= 16 && dirv < 16 + 361) { char c[16]; int idx = dirv - 16; sprintf(c, "C%d,%d", idx % 19 - 9, idx / 19 - 9); ADD(when, c); }
                    i += 2 + mm;
                } else {
                    if (i + 3 >= m) break;
                    unsigned long raw = ((b >> 5) & 7) | (p[i + 1] << 3) | (p[i + 2] << 11) | ((unsigned long)p[i + 3] << 19);
                    when += (raw & 0x7FFFFF) + 1; one[0] = IDX[(b >> 2) & 3]; ADD(when, one); i += 4;
                }
            }
            inTicks = (int)(best / 2) + 40;
        }
        free(s);
        if (!found) { nEv = 0; inTicks = 0; inLevel = 0; }       /* the file stays loaded: the Level box picks the level at Run input */
        if (!twsCount) { sprintf(err, "This .tws file has no solutions in it"); return 0; }
        return 1;
    }
    char *mv = strstr(s, "\"Moves\"");
    if (mv) {                     /* SuperCC JSON: one character per tick; lower case = a move made on that tick */
        char *q = strchr(mv + 7, '"'); if (!q) { sprintf(err, "Bad SuperCC file"); free(s); return 0; }
        q++; int t = 0; strcpy(inKind, "SuperCC"); inShiftAlways = 0;
        {   /* decode JSON escapes in place (mouse clicks are stored as control characters, written as \uXXXX) */
            char *o = q, *r = q;
            while (*r && *r != '"') {
                if (*r == '\\' && r[1]) {
                    r++;
                    if (*r == 'u') { unsigned v = 0; sscanf(r + 1, "%4x", &v); *o++ = (char)v; r += 5; continue; }
                    *o++ = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r == 'r' ? '\r' : *r == 'b' ? '\b' : *r == 'f' ? '\f' : *r; r++; continue;
                }
                *o++ = *r++;
            }
            *o = '"';
        }
        for (; *q && *q != '"'; q++, t++) {
            char c = *q, d = c == 'u' ? 'N' : c == 'd' ? 'S' : c == 'l' ? 'W' : c == 'r' ? 'E' : 0;
            if (c == 1 && q[1] && q[2]) {                  /* mouse click: marker, then x+9 and y+9 relative to Chip */
                if (nEv < MAXEV) { ev[nEv].n = t; sprintf(ev[nEv].k, "C%d,%d", (unsigned char)q[1] - 9, (unsigned char)q[2] - 9); nEv++; }
                q += 2; continue; }
            if (d && nEv < MAXEV) { ev[nEv].n = t; ev[nEv].k[0] = d; ev[nEv].k[1] = 0; nEv++; }
        }
        inTicks = t + 40; char *sd = strstr(s, "\"Seed\""); if (sd && (sd = strchr(sd + 6, '"'))) { inSeed = strtoul(sd + 1, NULL, 10); inHasSeed = 1; }
        free(s); return 1;
    }
    char *line = strtok(s, "\r\n");
    for (; line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;
        if (!strncmp(line, "level", 5)) { inLevel = atoi(line + 5); continue; }
        if (!strncmp(line, "ticks", 5)) { inTicks = atoi(line + 5); continue; }
        if (!strncmp(line, "seed", 4)) { inSeed = strtoul(line + 4, NULL, 0); inHasSeed = 1; continue; }
        if (!strncmp(line, "set", 3)) continue;
        if (!strncmp(line, "start", 5)) { inShiftAlways = !strstr(line, "auto"); continue; }
        if (isdigit((unsigned char)*line)) {
            int t = atoi(line); char *c = strchr(line, ':'); if (!c) continue; c++;
            char *tok = c; while (*tok) {
                while (*tok == ' ') tok++;
                if (!*tok) break;
                char word[16]; int k = 0; while (*tok && *tok != ' ' && k < 15) word[k++] = *tok++; word[k] = 0;
                if (k && nEv < MAXEV) { ev[nEv].n = t; strcpy(ev[nEv].k, word); nEv++; }
            }
        }
    }
    free(s);
    if (!inTicks) { for (int i = 0; i < nEv; i++) if (ev[i].n + 40 > inTicks) inTicks = ev[i].n + 40; }
    return 1;
}
static void script_keys(int t) {       /* deliver the keys recorded for "after t ticks" */
    for (int i = 0; i < nEv; i++) if (ev[i].n == t) {
        const char *k = ev[i].k;
        if (k[0] == 'C') { int dx = 0, dy = 0; sscanf(k + 1, "%d,%d", &dx, &dy); game_click(dx, dy); }
        else if (strchr("NSWE", k[0]) && !k[1]) key_sync(vk_of(k[0]));
    }
}

static void script_start(void) {      /* text inputs: Shift first (as the glitch pack recorder); TWS / SuperCC: only if the first key comes later */
    int first = 1 << 30; for (int i = 0; i < nEv; i++) if (ev[i].n < first) first = ev[i].n;
    if (inShiftAlways || first > 0) game_start();
}

/* ------------------------------------------------------------------ field table */
typedef struct { const char *name; int base, off, size; const char *desc; } Field;   /* base 0 = data segment, 1 = board */
static const Field F[] = {
    { "chips_left", 0, 0x1692, 2, "chips still needed" }, { "time_left", 0, 0x1694, 2, "seconds left" },
    { "key_blue", 0, 0x1682, 2, "blue keys" }, { "key_red", 0, 0x1684, 2, "red keys" }, { "key_green", 0, 0x1686, 2, "green keys" }, { "key_yellow", 0, 0x1688, 2, "yellow keys" },
    { "boot_flippers", 0, 0x168A, 2, "flippers" }, { "boot_fire", 0, 0x168C, 2, "fire boots" }, { "boot_skates", 0, 0x168E, 2, "ice skates" }, { "boot_suction", 0, 0x1690, 2, "suction boots" },
    { "rng_state", 0, 0x1498, 4, "random generator state" }, { "tick", 0, 0x064E, 2, "session tick counter" },
    { "level", 1, 0x800, 2, "level number" }, { "level_count", 1, 0x802, 2, "levels in the set" }, { "time_limit", 1, 0x804, 2, "time limit as loaded" },
    { "chips_required", 1, 0x806, 2, "chips required as loaded" }, { "chip_x", 1, 0x808, 2, "Chip x" }, { "chip_y", 1, 0x80A, 2, "Chip y" },
    { "chip_sliding", 1, 0x80C, 2, "Chip sliding" }, { "key_buffered", 1, 0x80E, 2, "refused key buffered" }, { "not_started", 1, 0x810, 2, "1 until the first key" },
    { "buffered_dx", 1, 0x812, 2, "buffered key dx" }, { "buffered_dy", 1, 0x814, 2, "buffered key dy" }, { "ice_seen / slide dx", 1, 0x818, 2, "slide direction (dx)" },
    { "slide_count", 1, 0x91E, 2, "slide list length" }, { "monster_count", 1, 0x928, 2, "monster list length" },
    { "trap_count", 1, 0x93C, 2, "trap wires" }, { "clone_count", 1, 0x946, 2, "clone wires" },
    { "view_x", 1, 0xA24, 2, "view origin x" }, { "view_y", 1, 0xA26, 2, "view origin y" }, { "chip_moves", 1, 0xA34, 2, "Chip square changes" },
    { "chip_idle", 1, 0xA3E, 2, "Chip idle counter" }, { "chip_step_used", 1, 0xA40, 2, "Chip moved this step" },
};
#define NF (int)(sizeof F / sizeof F[0])
static unsigned long fget(int i) {
    unsigned char b[4] = { 0 }; mem_read(0xFFFF, (F[i].base ? B : 0) + F[i].off, b, F[i].size);
    return F[i].size == 4 ? (unsigned long)b[0] | b[1] << 8 | b[2] << 16 | (unsigned long)b[3] << 24 : (unsigned long)(b[0] | b[1] << 8);
}
static int fset(int i, unsigned long v, char *err) {
    unsigned char b[4] = { v, v >> 8, v >> 16, v >> 24 }; return mem_write(0xFFFF, (F[i].base ? B : 0) + F[i].off, b, F[i].size, err);
}
static int fidx(const char *n) { for (int i = 0; i < NF; i++) if (!strcmp(F[i].name, n)) return i; return -1; }

/* ------------------------------------------------------------------ data map recorder: one JSON line per tick */
static FILE *recF; static unsigned char recTop[1024], recBot[1024]; static char recPath[MAX_PATH];
static void rec_list(FILE *f, unsigned cntoff, unsigned hoff, unsigned limit) {
    unsigned cnt = rd16(0xFFFF, B + cntoff), hd = rd16(0xFFFF, B + hoff); if (cnt > limit) cnt = limit;
    unsigned char *raw = malloc(cnt * 11 + 1); fputc('[', f);
    if (hd && cnt && mem_read(hd, 0, raw, cnt * 11))
        for (unsigned i = 0; i < cnt; i++) { unsigned char *e = raw + i * 11;
            fprintf(f, "%s[%u,%u,%u,%d,%d,%u]", i ? "," : "", e[0], e[1] | e[2] << 8, e[3] | e[4] << 8, (short)(e[5] | e[6] << 8), (short)(e[7] | e[8] << 8), e[9] | e[10] << 8); }
    fputc(']', f); free(raw);
}
static void rec_tick(FILE *f, int n, const char *st) {
    unsigned char t[1024], b[1024], d[0x60]; char hx[0x60 * 2 + 1];
    if (!f) return;
    mem_read(0xFFFF, B, t, 1024); mem_read(0xFFFF, B + 0x400, b, 1024); mem_read(0xFFFF, B + 0x800, d, 0x60);
    for (int i = 0; i < 0x60; i++) sprintf(hx + 2 * i, "%02X", d[i]);
    fprintf(f, "{\"n\":%d,\"status\":\"%s\",\"chip\":[%u,%u],\"chips_left\":%u,\"time_left\":%u,\"keys\":[%u,%u,%u,%u],\"boots\":[%u,%u,%u,%u],"
               "\"rng\":%lu,\"tick_counter\":%u,\"data_800\":\"%s\",\"monsters\":", n, st, rd16(0xFFFF, B + 0x808), rd16(0xFFFF, B + 0x80A),
            rd16(0xFFFF, 0x1692), rd16(0xFFFF, 0x1694), rd16(0xFFFF, 0x1682), rd16(0xFFFF, 0x1684), rd16(0xFFFF, 0x1686), rd16(0xFFFF, 0x1688),
            rd16(0xFFFF, 0x168A), rd16(0xFFFF, 0x168C), rd16(0xFFFF, 0x168E), rd16(0xFFFF, 0x1690), fget(fidx("rng_state")), rd16(0xFFFF, 0x064E), hx);
    rec_list(f, 0x928, 0x92C, 1152); fputs(",\"slides\":", f); rec_list(f, 0x91E, 0x922, 1152);
    for (int L = 0; L < 2; L++) {
        unsigned char *now = L ? b : t, *was = L ? recBot : recTop; int k = 0;
        fprintf(f, ",\"%s\":{", L ? "bottom" : "top");
        for (int i = 0; i < 1024; i++) if (now[i] != was[i] || n == 0) fprintf(f, "%s\"%d\":%u", k++ ? "," : "", i, now[i]);
        fputc('}', f); memcpy(was, now, 1024);
    }
    fputs("}\n", f); fflush(f);
}
static void rec_open(const char *path, const char *about) {
    if (recF) fclose(recF);
    recF = fopen(path, "w"); if (!recF) return; strcpy(recPath, path);
    unsigned lv = rd16(0xFFFF, B + 0x800);
    fprintf(recF, "{\"format\":\"mspptool data map 1\",\"level\":%u,\"title\":\"%s\",\"source\":\"", lv, lv <= MAXLEV ? levTitle[lv] : "");
    for (const char *p = about; *p; p++) { if (*p == '\\' || *p == '"') fputc('\\', recF); fputc(*p, recF); }
    fputs("\",\"fields\":\"n = ticks run; chip [x,y]; keys B,R,G,Y; boots flippers,fire,skates,suction; data_800 = 96 bytes from board+0x800 "
          "(row 32 is x = byte index); monsters/slides entries [tile,x,y,dx,dy,flag]; top/bottom = squares (index y*32+x) changed since the previous line; "
          "line n=0 holds the full layers\"}\n", recF);
    rec_tick(recF, 0, "start");
}
static void rec_close(void) { if (recF) { fclose(recF); recF = NULL; } }

/* ------------------------------------------------------------------ test mode */
static int run_test(int argc, char **argv) {
    char err[512];
    strcpy(gameDir, argv[2]); strcpy(datPath, argv[3]); testOut = fopen(argv[5], "w"); if (!testOut) return 2;
    if (argc >= 7) wantLevel = atoi(argv[6]);
    if (!setup_work(err)) { fprintf(testOut, "error %s\n", err); fclose(testOut); return 1; }
    if (!load_input(argv[4], err)) { fprintf(testOut, "error %s\n", err); fclose(testOut); return 1; }
    if (strstr(inKind, "TWS") && !inLevel) { fprintf(testOut, "error this .tws file has no solution for level %d\n", wantLevel); fclose(testOut); return 1; }
    if (!game_launch(inLevel ? inLevel : wantLevel, err)) { fprintf(testOut, "error %s\n", err); game_kill(); ini_restore(); fclose(testOut); return 1; }
    fset(fidx("rng_state"), inHasSeed ? inSeed : 0, NULL);
    fclose(testOut); testOut = NULL; rec_open(argv[5], argv[4]);
    script_start(); tickNo = 0;
    for (int t = 0; t < inTicks; t++) {
        script_keys(t); game_tick();
        const char *st = game_status(); rec_tick(recF, t + 1, st);
        if (strcmp(st, "playing")) break;
    }
    rec_close(); game_kill(); ini_restore(); return 0;
}

/* ------------------------------------------------------------------ GUI */
enum { ID_SETUP = 100, ID_DAT, ID_LAUNCH, ID_LEVEL, ID_GOTO, ID_RESTART, ID_PLAY, ID_STEP, ID_STEP10, ID_SPEED, ID_OK,
       ID_LOADIN, ID_RUNIN, ID_HEX, ID_LAYER_TOP, ID_LAYER_BOT, ID_CELLV, ID_CELLW, ID_INV0 = 200, ID_INVAPPLY = 230, ID_INVREAD,
       ID_FIELD, ID_FIELDV, ID_FIELDSET, ID_LISTSEL, ID_LIST, ID_SEED, ID_TIMER = 1, ID_REFRESH = 2,
       ID_RECORD = 240, ID_SAVEKEYS, ID_LVPREV = 250, ID_LVNEXT, ID_SOLPREV, ID_SOLNEXT, ID_LBL_LEVEL = 400, ID_LBL_SEED, ID_LBL_FIELD, ID_LBL_INV0 = 410, ID_HBLOCK = 300, ID_HTEXT, ID_HOFF, ID_HBYTES, ID_HWRITE, ID_HREFRESH, ID_HAUTO, ID_HPAGE };
static HWND hMain, hGrid, hStatus, hInfo, hPlay, hLevel, hSpeed, hCellV, hCellLbl, hInv[10], hField, hFieldV, hListSel, hList, hInLbl, hSeed, hData, hDatLbl;
static HFONT fMono, fSmall, fUi;
static int text_w(HWND c); static int fontH(void); static void place(HWND c, int x, int y, int w, int hh); static void layout(int W, int H);
static int perKey, playing, layerBot, selX = -1, selY = -1, speedMs = 110;
static char keyAt[200000]; static int keyMax;   /* keys played through the tool, per tick (for Save keys) */
static HWND hRecord;
static unsigned char top[1024], bot[1024];
static const char *INV[10] = { "key_blue", "key_red", "key_green", "key_yellow", "boot_flippers", "boot_fire", "boot_skates", "boot_suction", "chips_left", "time_left" };
static const char *INVL[10] = { "Blue keys", "Red keys", "Green keys", "Yellow keys", "Flippers", "Fire boots", "Skates", "Suction", "Chips left", "Time left" };

static void status(const char *fmt, ...) {
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (testMode) { if (testOut) fprintf(testOut, "# %s\n", b); return; }
    if (hStatus) SetWindowTextA(hStatus, b);
}
static COLORREF tile_color(int t) {
    if (t == 0) return RGB(250, 250, 250); if (t == 1) return RGB(150, 150, 150); if (t == 2) return RGB(255, 230, 120);
    if (t == 3) return RGB(140, 180, 255); if (t == 4) return RGB(255, 140, 120); if (t == 0x15) return RGB(120, 230, 140);
    if (t >= 0x40 && t < 0x64) return RGB(255, 190, 110); if (t >= 0x64 && t < 0x6C) return RGB(170, 240, 170);
    if (t >= 0x6C && t < 0x70) return RGB(255, 255, 0); if (t > 0x6F) return RGB(215, 170, 255);
    return RGB(230, 230, 215);
}
static int CELL = 17; static HFONT fGrid;
static LRESULT CALLBACK grid_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); unsigned char *L = layerBot ? bot : top;
        int cx = gProc ? (int)rd16(0xFFFF, B + 0x808) : -1, cy = gProc ? (int)rd16(0xFFFF, B + 0x80A) : -1;
        SelectObject(dc, fGrid ? fGrid : fSmall); SetBkMode(dc, TRANSPARENT);
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
            RECT r = { x * CELL, y * CELL, x * CELL + CELL - 1, y * CELL + CELL - 1 }; int t = L[y * 32 + x];
            HBRUSH br = CreateSolidBrush(tile_color(t)); FillRect(dc, &r, br); DeleteObject(br);
            char s[4]; sprintf(s, "%02X", t); DrawTextA(dc, s, 2, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if ((x == cx && y == cy) || (x == selX && y == selY)) {
                HBRUSH fr = CreateSolidBrush(x == selX && y == selY ? RGB(0, 0, 255) : RGB(220, 0, 0)); FrameRect(dc, &r, fr); DeleteObject(fr);
            }
        }
        EndPaint(h, &ps); return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        SetFocus(h);
        selX = LOWORD(l) / CELL; selY = HIWORD(l) / CELL; if (selX > 31 || selY > 31) { selX = selY = -1; return 0; }
        char s[64]; sprintf(s, "Square (%d,%d): top %02X, bottom %02X. New value:", selX, selY, top[selY * 32 + selX], bot[selY * 32 + selX]);
        SetWindowTextA(hCellLbl, s); sprintf(s, "%02X", (layerBot ? bot : top)[selY * 32 + selX]); SetWindowTextA(hCellV, s);
        InvalidateRect(h, NULL, FALSE); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static char *listBuf; static size_t listLen, listCap;
static void list_add(const char *line) {
    size_t n = strlen(line) + 1;
    if (listLen + n > listCap) { listCap = (listLen + n) * 2 + 4096; listBuf = realloc(listBuf, listCap); }
    memcpy(listBuf + listLen, line, n); listLen += n;
}
static void fill_list(void) {
    int which = (int)SendMessageA(hListSel, CB_GETCURSEL, 0, 0); char s[160];
    static char *shown; static size_t shownLen; static int shownWhich = -1;
    listLen = 0;
    if (gProc && which <= 1) {
        unsigned cnt = rd16(0xFFFF, B + (which ? 0x91E : 0x928)), hd = rd16(0xFFFF, B + (which ? 0x922 : 0x92C));
        if (cnt > 1200) cnt = 1200;
        unsigned char *raw = malloc(cnt * 11 + 1);
        if (hd && cnt && mem_read(hd, 0, raw, cnt * 11)) for (unsigned i = 0; i < cnt; i++) {
            unsigned char *e = raw + i * 11;
            sprintf(s, "%3u: tile %02X  at (%u,%u)  dir (%d,%d)  flag %u", i, e[0], e[1] | e[2] << 8, e[3] | e[4] << 8, (short)(e[5] | e[6] << 8), (short)(e[7] | e[8] << 8), e[9] | e[10] << 8);
            list_add(s);
        }
        free(raw);
    } else if (gProc) {
        int tr = which == 2; unsigned cnt = rd16(0xFFFF, B + (tr ? 0x93C : 0x946)), hd = rd16(0xFFFF, B + (tr ? 0x940 : 0x94A)), sz = tr ? 10 : 8;
        if (cnt > 1000) cnt = 1000;
        unsigned char *raw = malloc(cnt * sz + 1);
        if (hd && cnt && mem_read(hd, 0, raw, cnt * sz)) for (unsigned i = 0; i < cnt; i++) {
            unsigned char *e = raw + i * sz; unsigned v[5]; for (int k = 0; k < 5; k++) v[k] = e[2 * k] | e[2 * k + 1] << 8;
            if (tr) sprintf(s, "%3u: button (%u,%u) -> trap (%u,%u)  holds %u", i, v[0], v[1], v[2], v[3], v[4]);
            else sprintf(s, "%3u: button (%u,%u) -> clone machine (%u,%u)%s", i, v[0], v[1], v[2], v[3], v[3] >= 32 ? "  <- outside the map (data reset)" : "");
            list_add(s);
        }
        free(raw);
    }
    if (which == shownWhich && listLen == shownLen && (!listLen || !memcmp(shown, listBuf, listLen))) return;   /* unchanged */
    int top = (int)SendMessageA(hList, LB_GETTOPINDEX, 0, 0), sel = (int)SendMessageA(hList, LB_GETCURSEL, 0, 0);
    if (which != shownWhich) top = 0, sel = -1;
    SendMessageA(hList, WM_SETREDRAW, FALSE, 0); SendMessageA(hList, LB_RESETCONTENT, 0, 0);
    for (size_t o = 0; o < listLen; o += strlen(listBuf + o) + 1) SendMessageA(hList, LB_ADDSTRING, 0, (LPARAM)(listBuf + o));
    SendMessageA(hList, LB_SETTOPINDEX, top, 0); if (sel >= 0) SendMessageA(hList, LB_SETCURSEL, sel, 0);
    SendMessageA(hList, WM_SETREDRAW, TRUE, 0); InvalidateRect(hList, NULL, TRUE);
    shown = realloc(shown, listLen + 1); if (listLen) memcpy(shown, listBuf, listLen); shownLen = listLen; shownWhich = which;
}
static void fill_data(void) {     /* the 32 data bytes that clone wires to (x,32) reach: board + 0x800 + x */
    static const char *lab[32] = { "level", "", "level count", "", "time limit", "", "chips required", "", "Chip x", "", "Chip y", "", "sliding", "",
        "key buffered", "", "not started", "", "buffered dx", "", "buffered dy", "", "(0x816)", "", "slide dx", "", "(0x81A)", "", "(0x81C)", "", "(0x81E)", "" };
    char s[2048] = "", line[96]; unsigned char d[32]; if (!gProc) { SetWindowTextA(hData, ""); return; }
    mem_read(0xFFFF, B + 0x800, d, 32);
    strcat(s, "Row 32 = board + 0x800 + x  (x: byte, word value, meaning)\r\n");
    for (int x = 0; x < 32; x += 2) {
        sprintf(line, "x=%2d,%2d: %02X %02X  = %5u  %s\r\n", x, x + 1, d[x], d[x + 1], d[x] | d[x + 1] << 8, lab[x]); strcat(s, line);
    }
    static char last[2048];
    if (!strcmp(s, last)) return;
    strcpy(last, s);
    int first = (int)SendMessageA(hData, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageA(hData, WM_SETREDRAW, FALSE, 0); SetWindowTextA(hData, s); SendMessageA(hData, EM_LINESCROLL, 0, first);
    SendMessageA(hData, WM_SETREDRAW, TRUE, 0); InvalidateRect(hData, NULL, TRUE);
}
static int invDirty[10], invSetting;
static void refresh(int full) {
    char s[1024];
    if (!gProc) { SetWindowTextA(hInfo, "No game running. Use Setup, then Launch."); return; }
    mem_read(0xFFFF, B, top, 1024); mem_read(0xFFFF, B + 0x400, bot, 1024);
    unsigned lv = rd16(0xFFFF, B + 0x800); const char *st = game_status();
    sprintf(s, "Level %u: %s\r\nTick %d   status: %s%s\r\nChip (%u,%u)  sliding %u  chips left %u  time left %u\r\n"
               "Keys B/R/G/Y %u/%u/%u/%u   Boots F/Fi/S/Su %u/%u/%u/%u\r\nMonsters %u  sliders %u  traps %u  clones %u\r\nRandom state %08lX",
            lv, lv <= MAXLEV ? levTitle[lv] : "?", tickNo, st, scriptOn ? "   (input file running)" : "", rd16(0xFFFF, B + 0x808), rd16(0xFFFF, B + 0x80A),
            rd16(0xFFFF, B + 0x80C), rd16(0xFFFF, 0x1692), rd16(0xFFFF, 0x1694), rd16(0xFFFF, 0x1682), rd16(0xFFFF, 0x1684), rd16(0xFFFF, 0x1686), rd16(0xFFFF, 0x1688),
            rd16(0xFFFF, 0x168A), rd16(0xFFFF, 0x168C), rd16(0xFFFF, 0x168E), rd16(0xFFFF, 0x1690), rd16(0xFFFF, B + 0x928), rd16(0xFFFF, B + 0x91E),
            rd16(0xFFFF, B + 0x93C), rd16(0xFFFF, B + 0x946), fget(fidx("rng_state")));
    SetWindowTextA(hInfo, s);
    HWND foc = GetFocus(); char cur[32];      /* live values, except boxes the user is typing in (until Apply or Read) */
    if (full) memset(invDirty, 0, sizeof invDirty);
    invSetting = 1;
    for (int i = 0; i < 10; i++) if (full || (hInv[i] != foc && !invDirty[i])) {
        sprintf(s, "%lu", fget(fidx(INV[i]))); GetWindowTextA(hInv[i], cur, sizeof cur); if (strcmp(cur, s)) SetWindowTextA(hInv[i], s); }
    invSetting = 0;
    InvalidateRect(hGrid, NULL, FALSE); fill_data(); fill_list();
}
static int begun;                       /* the level clock was started for this attempt */
static unsigned long attemptSeed;       /* MSCC's random state when this attempt started (tick 0): what "Save keys" writes */
static void mark_attempt(void) {   /* a new attempt (launch, restart, death, after a win): start it exactly like a fresh launch */
    fset(fidx("tick"), 0, NULL);      /* MSCC does not reset its tick counter on a restart; creatures and Chip's moves depend on it */
    attemptSeed = fget(fidx("rng_state"));
}
static void live_begin(void) {          /* first tick of live play: start the clock, open a data map if Record is on */
    if (begun) return; begun = 1;
    game_start();
    if (IsDlgButtonChecked(hMain, ID_RECORD) && !recF) {
        char p[MAX_PATH]; SYSTEMTIME tm; GetLocalTime(&tm); sprintf(p, "%s\\records", toolDir); CreateDirectoryA(p, NULL);
        sprintf(p, "%s\\records\\live_level%u_%04d%02d%02d_%02d%02d%02d.jsonl", toolDir, rd16(0xFFFF, B + 0x800), tm.wYear, tm.wMonth, tm.wDay, tm.wHour, tm.wMinute, tm.wSecond);
        rec_open(p, "live play"); status("Recording the data map to %s", p);
    }
}
static void game_front(void) { if (gGame && !getenv("MSPP_NOFRONT")) { SetForegroundWindow(gGame); BringWindowToTop(gGame); } }
static void log_key(int vk) {         /* keys played live, per tick, for "Save keys as SuperCC JSON" */
    if (scriptOn || tickNo >= (int)sizeof keyAt) return;
    if (!keyAt[tickNo]) keyAt[tickNo] = vk == VK_UP ? 'u' : vk == VK_DOWN ? 'd' : vk == VK_LEFT ? 'l' : 'r';
    if (tickNo > keyMax) keyMax = tickNo;
}
static HHOOK kbHook;
static LRESULT CALLBACK kb_hook(int code, WPARAM w, LPARAM l) {   /* arrow keys typed into the game window */
    if (code == HC_ACTION && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN) && gGame && GetForegroundWindow() == gGame) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)l; int vk = (int)k->vkCode;
        if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) { log_key(vk); if (perKey) PostMessageA(hMain, WM_APP + 5, vk, 0); }
        else if (vk == VK_SPACE && perKey) PostMessageA(hMain, WM_APP + 5, vk, 0);
    }
    return CallNextHookEx(kbHook, code, w, l);
}
static void set_play(int on) {     /* in "1 move per key" mode only an input file plays on a timer (at x1) */
    if (perKey && !scriptOn) on = 0; playing = on; SetWindowTextA(hPlay, on ? "Pause" : "Play"); KillTimer(hMain, ID_TIMER);
    if (on) SetTimer(hMain, ID_TIMER, perKey ? 110 : speedMs ? speedMs : 10, NULL); }
static int endedBox;      /* a death / end box was seen: when the game is playing again, MSCC has restarted the level */
static int endWonLevel;      /* level number of a win box seen: MSCC goes on to the next level after it, the tool goes back */
static void level_restarted(void) {
    if (endWonLevel) {
        int won = endWonLevel; endWonLevel = 0;
        if ((int)rd16(0xFFFF, B + 0x800) == won + 1) {     /* MSCC moved on: Level > Previous brings the won level back */
            int id = gGame ? 111 : 0;     /* Level > Previous */
            if (id) { DWORD_PTR rv; SendMessageTimeoutA(gGame, WM_COMMAND, id, 0, SMTO_NORMAL, 2000, &rv); Sleep(100); remap(); }
        }
    }
    rec_close(); tickNo = 0; scriptOn = 0; memset(keyAt, 0, sizeof keyAt); keyMax = 0; endedBox = 0; begun = 0; mark_attempt();
    if (perKey) live_begin();
    status("The game restarted the level: tick count and key log start again from 0.");
}
static int do_tick(void) {
    if (!gProc) return 0;
    const char *gs = game_status();
    if (strcmp(gs, "playing")) { if (!strcmp(gs, "won")) endWonLevel = rd16(0xFFFF, B + 0x800); endedBox = 1; set_play(0); status("A game message box is showing (press OK on dialog)."); return 0; }
    if (endedBox) { level_restarted(); live_begin(); }
    if (scriptOn) { if (scriptPos >= inTicks) { scriptOn = 0; set_play(0); if (recF) { rec_close(); status("Input file finished after %d ticks. Data map saved: %s", inTicks, recPath); } else status("Input file finished after %d ticks.", inTicks); return 0; } script_keys(scriptPos); scriptPos++; }
    int ok = game_tick(); if (!ok) status("The game did not answer the tick (a message box may be open).");
    if (ok && getenv("MSPP_KEYSAVE")) {        /* test switch: keep the logged keys in a file */
        FILE *kf = fopen(getenv("MSPP_KEYSAVE"), "w"); if (kf) { for (int i = 0; i <= keyMax; i++) fputc(keyAt[i] ? keyAt[i] : '-', kf); fclose(kf); }
    }
    if (ok && recF) { const char *st = game_status(); rec_tick(recF, tickNo, st); if (strcmp(st, "playing")) { rec_close(); status("Level ended (%s). Data map saved: %s", st, recPath); } }
    return ok;
}
static int ask_file(HWND o, char *out, const char *filter, int save) {
    OPENFILENAMEA ofn; char buf[MAX_PATH]; strcpy(buf, out); memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = o; ofn.lpstrFilter = filter; ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn)) return 0; strcpy(out, buf); return 1;
}
static void in_label(void) {        /* the input file line: file, kind, and for a .tws whether the Level box's level has a solution */
    char t[MAX_PATH + 200], lv[16]; if (!inputPath[0]) { SetWindowTextA(hInLbl, "(no input file)"); return; }
    const char *nm = strrchr(inputPath, '\\'); nm = nm ? nm + 1 : inputPath;
    if (strstr(inKind, "TWS")) {
        GetWindowTextA(hLevel, lv, sizeof lv); int L = atoi(lv);
        if (L > 0 && L <= MAXLEV && twsHas[L]) sprintf(t, "%s  (%s, %d levels)   Level %d: has a solution", nm, inKind, twsCount, L);
        else sprintf(t, "%s  (%s, %d levels)   Level %d: NO solution in this file", nm, inKind, twsCount, L);
    } else sprintf(t, "%s  (%s, level %d, %d ticks, %d key events)", nm, inKind, inLevel, inTicks, nEv);
    SetWindowTextA(hInLbl, t);
}
static int launch_level(int level) {    /* always a fresh game: the old one is closed before its files are replaced */
    char err[512];
    if (!gameDir[0]) { status("Choose your MSCC folder first (Setup)."); return 0; }
    set_play(0); scriptOn = 0; rec_close(); game_kill(); Sleep(300);
    status("Preparing the working copy...");
    int ok = 0; for (int i = 0; i < 10 && !(ok = setup_work(err)); i++) Sleep(300);   /* the old game may take a moment to let go of its files */
    if (!ok) { status("%s", err); refresh(1); return 0; }
    status("Starting the game at level %d...", level);
    if (!game_launch(level, err)) { status("%s", err); return 0; }
    char sd[32]; GetWindowTextA(hSeed, sd, sizeof sd); if (sd[0]) fset(fidx("rng_state"), strtoul(sd, NULL, 0), NULL);
    tickNo = 0; scriptOn = 0; endedBox = 0; begun = 0; set_play(0); mark_attempt(); memset(keyAt, 0, sizeof keyAt); keyMax = 0;
    char s[16]; sprintf(s, "%u", rd16(0xFFFF, B + 0x800)); SetWindowTextA(hLevel, s);
    SetForegroundWindow(hMain); SetFocus(hGrid);
    status("Game running (paused). Level set has %d levels. Press Play, then use the arrow keys here (or in the game window).", nLevels);
    refresh(1); return 1;
}

/* ---- hex view window */
static HWND hHex, hHBlock, hHText, hHOff, hHBytes, hHAuto, hHPage; static int hexSel[1100], nHexSel;
static const char *hex_name(int i, char *s) {
    if (hexSel[i] == -1) return strcpy(s, "Data segment (all non-stack)");
    if (hexSel[i] == -2) return strcpy(s, "Board top layer (board + 0x000)");
    if (hexSel[i] == -3) return strcpy(s, "Board bottom layer (board + 0x400)");
    if (hexSel[i] == -4) return strcpy(s, "Level data after the board (board + 0x800..0xAFF)");
    if (hexSel[i] == -5) return strcpy(s, "Monster list"); if (hexSel[i] == -6) return strcpy(s, "Slide list");
    if (hexSel[i] == -7) return strcpy(s, "Trap wires"); if (hexSel[i] == -8) return strcpy(s, "Clone wires");
    Block *b = &blocks[hexSel[i]]; sprintf(s, "Block %04X type %u size %lu", b->handle, b->type, b->size); return s;
}
static void hex_region(int i, unsigned *handle, unsigned long *off, unsigned long *len) {
    int k = hexSel[i]; *handle = 0xFFFF; *off = 0; *len = 0;
    if (k == -1) { *len = blocks[dsIdx].size; }
    else if (k == -2) { *off = B; *len = 0x400; } else if (k == -3) { *off = B + 0x400; *len = 0x400; } else if (k == -4) { *off = B + 0x800; *len = 0x300; }
    else if (k <= -5) {
        static const unsigned hoff[4] = { 0x92C, 0x922, 0x940, 0x94A }, coff[4] = { 0x92A, 0x920, 0x93E, 0x948 }, esz[4] = { 11, 11, 10, 8 };
        int j = -5 - k; *handle = rd16(0xFFFF, B + hoff[j]); *len = (unsigned long)rd16(0xFFFF, B + coff[j]) * esz[j];
        int bi = blk_of(*handle); if (bi < 0) *len = 0; else if (*len > blocks[bi].size) *len = blocks[bi].size;
    } else { *handle = blocks[k].handle; *len = blocks[k].size; }
}
static void hex_fill_blocks(void) {
    char s[128]; int cur = (int)SendMessageA(hHBlock, CB_GETCURSEL, 0, 0);
    SendMessageA(hHBlock, CB_RESETCONTENT, 0, 0); nHexSel = 0;
    for (int k = -1; k >= -8; k--) hexSel[nHexSel++] = k;
    for (int i = 0; i < nBlocks && nHexSel < 1100; i++) hexSel[nHexSel++] = i;
    for (int i = 0; i < nHexSel; i++) SendMessageA(hHBlock, CB_ADDSTRING, 0, (LPARAM)hex_name(i, s));
    SendMessageA(hHBlock, CB_SETCURSEL, cur < 0 || cur >= nHexSel ? 0 : cur, 0);
}
static void hex_show(void) {
    if (!gProc || !hHex) return;
    int i = (int)SendMessageA(hHBlock, CB_GETCURSEL, 0, 0); if (i < 0) return;
    unsigned h; unsigned long off, len; hex_region(i, &h, &off, &len);
    int page = (int)SendMessageA(hHPage, CB_GETCURSEL, 0, 0); if (page < 0) page = 0;
    unsigned long from = (unsigned long)page * 0x1000; if (from >= len) from = 0;
    unsigned long n = len - from > 0x1000 ? 0x1000 : len - from;
    unsigned char *d = malloc(n + 16); mem_read(h, off + from, d, n);
    char *s = malloc(n / 16 * 80 + 400), *p = s;
    p += sprintf(p, "%s: %lu bytes from offset %04lX%s%s\r\n", h == 0xFFFF ? "data segment" : "block", len, off, len > 0x1000 ? " (shown in 4 KB pages)" : "",
                 h == 0xFFFF ? "; the game's stack range is shown as --" : "");
    for (unsigned long r = 0; r < n; r += 16) {
        p += sprintf(p, "%05lX: ", from + r);
        for (int k = 0; k < 16; k++) {
            unsigned long a = off + from + r + k; int stk = h == 0xFFFF && a >= stackTop && a < stackBot;   /* the game's stack is not shown */
            p += r + k >= n ? sprintf(p, "   ") : stk ? sprintf(p, "-- ") : sprintf(p, "%02X ", d[r + k]);
        }
        p += sprintf(p, " ");
        for (int k = 0; k < 16 && r + k < n; k++) { unsigned long a = off + from + r + k; int stk = h == 0xFFFF && a >= stackTop && a < stackBot;
            *p++ = stk ? ' ' : d[r + k] >= 32 && d[r + k] < 127 ? d[r + k] : '.'; }
        p += sprintf(p, "\r\n");
    }
    *p = 0;
    int first = (int)SendMessageA(hHText, EM_GETFIRSTVISIBLELINE, 0, 0);
    SetWindowTextA(hHText, s); SendMessageA(hHText, EM_LINESCROLL, 0, first);
    free(d); free(s);
    SendMessageA(hHPage, CB_RESETCONTENT, 0, 0);
    for (unsigned long q = 0; q < (len ? len : 1); q += 0x1000) { char t[48]; sprintf(t, "%05lX-%05lX", q, (q + 0x1000 < len ? q + 0x1000 : len) - 1); SendMessageA(hHPage, CB_ADDSTRING, 0, (LPARAM)t); }
    SendMessageA(hHPage, CB_SETCURSEL, page, 0);
}
static void hex_write(void) {
    char a[64], b[1024], err[128] = ""; unsigned char d[512]; int n = 0;
    GetWindowTextA(hHOff, a, sizeof a); GetWindowTextA(hHBytes, b, sizeof b);
    for (char *p = b; *p && n < 512;) { while (*p == ' ' || *p == ',') p++; if (!*p) break; d[n++] = (unsigned char)strtoul(p, &p, 16); }
    int i = (int)SendMessageA(hHBlock, CB_GETCURSEL, 0, 0); unsigned h; unsigned long off, len; hex_region(i, &h, &off, &len);
    unsigned long o = strtoul(a, NULL, 16);
    if (!n) { status("Type hex bytes to write, e.g. 69 00"); return; }
    if (o + n > len) { status("Offset %lX + %d bytes is past the end of this view (%lu bytes)", o, n, len); return; }
    if (mem_write(h, off + o, d, n, err)) status("Wrote %d byte(s) at offset %lX.", n, o); else status("Write %s", err);
    hex_show(); refresh(1);
}
static LRESULT CALLBACK hex_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        int id = LOWORD(w);
        if ((id == ID_HBLOCK || id == ID_HPAGE) && HIWORD(w) == CBN_SELCHANGE) { if (id == ID_HBLOCK) SendMessageA(hHPage, CB_SETCURSEL, 0, 0); hex_show(); }
        if (id == ID_HREFRESH) { remap(); hex_fill_blocks(); hex_show(); }
        if (id == ID_HWRITE) hex_write();
        return 0;
    }
    if (m == WM_SIZE) {
        int W = LOWORD(l), H = HIWORD(l), fh = fontH(), bh = fh + 12, eh = fh + 8, g = 6, y = 8;
        HWND rf = GetDlgItem(h, ID_HREFRESH), wr = GetDlgItem(h, ID_HWRITE), lo = GetDlgItem(h, 391), lb = GetDlgItem(h, 392);
        int wa = text_w(hHAuto) + 30, wrf = text_w(rf) + 24, wp = 170;
        place(hHAuto, W - 8 - wa, y + 2, wa, bh - 4); place(rf, W - 8 - wa - g - wrf, y, wrf, bh);
        place(hHPage, W - 8 - wa - 2 * g - wrf - wp, y + (bh - eh) / 2, wp, 400);
        place(hHBlock, 8, y + (bh - eh) / 2, W - 8 - wa - 3 * g - wrf - wp - 8, 400);
        y += bh + g;
        int wlo = text_w(lo) + 6, wlb = text_w(lb) + 6, ww = text_w(wr) + 24;
        place(lo, 8, y + (bh - fh) / 2, wlo, fh + 2); place(hHOff, 8 + wlo, y + (bh - eh) / 2, 90, eh);
        place(lb, 8 + wlo + 90 + 2 * g, y + (bh - fh) / 2, wlb, fh + 2);
        place(wr, W - 8 - ww, y, ww, bh);
        place(hHBytes, 8 + wlo + 90 + 2 * g + wlb, y + (bh - eh) / 2, W - 8 - ww - g - (8 + wlo + 90 + 2 * g + wlb), eh);
        y += bh + g;
        place(hHText, 8, y, W - 16, H - y - 8);
        return 0;
    }
    if (m == WM_CLOSE) { DestroyWindow(h); hHex = NULL; return 0; }
    return DefWindowProcA(h, m, w, l);
}
static void open_hex(void) {
    if (hHex) { SetForegroundWindow(hHex); return; }
    if (!gProc) { status("Launch the game first."); return; }
    hHex = CreateWindowA("MsppHex", "MSCC data (hex)", WS_OVERLAPPEDWINDOW, 60, 60, 960, 720, NULL, NULL, GetModuleHandle(NULL), NULL);
    hHBlock = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 8, 8, 380, 400, hHex, (HMENU)ID_HBLOCK, NULL, NULL);
    hHPage = CreateWindowA("COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 396, 8, 130, 400, hHex, (HMENU)ID_HPAGE, NULL, NULL);
    CreateWindowA("BUTTON", "Refresh", WS_CHILD | WS_VISIBLE, 534, 8, 80, 24, hHex, (HMENU)ID_HREFRESH, NULL, NULL);
    hHAuto = CreateWindowA("BUTTON", "Auto refresh", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 620, 8, 120, 24, hHex, (HMENU)ID_HAUTO, NULL, NULL);
    CreateWindowA("STATIC", "Offset (hex):", WS_CHILD | WS_VISIBLE, 8, 42, 80, 20, hHex, (HMENU)391, NULL, NULL);
    hHOff = CreateWindowA("EDIT", "0", WS_CHILD | WS_VISIBLE | WS_BORDER, 90, 40, 70, 22, hHex, (HMENU)ID_HOFF, NULL, NULL);
    CreateWindowA("STATIC", "Bytes (hex):", WS_CHILD | WS_VISIBLE, 168, 42, 75, 20, hHex, (HMENU)392, NULL, NULL);
    hHBytes = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 246, 40, 290, 22, hHex, (HMENU)ID_HBYTES, NULL, NULL);
    CreateWindowA("BUTTON", "Write", WS_CHILD | WS_VISIBLE, 542, 39, 70, 24, hHex, (HMENU)ID_HWRITE, NULL, NULL);
    hHText = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 8, 72, 730, 520, hHex, (HMENU)ID_HTEXT, NULL, NULL);
    for (HWND c = GetWindow(hHex, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) SendMessageA(c, WM_SETFONT, (WPARAM)fUi, 0);
    SendMessageA(hHText, WM_SETFONT, (WPARAM)fMono, 0);
    { RECT rc; GetClientRect(hHex, &rc); SendMessageA(hHex, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom)); } ShowWindow(hHex, SW_SHOW);
    hex_fill_blocks(); hex_show();
}

static void set_fonts(HWND parent) {
    HWND c = GetWindow(parent, GW_CHILD);
    for (; c; c = GetWindow(c, GW_HWNDNEXT)) if (c != hGrid && c != hData && c != hList) SendMessageA(c, WM_SETFONT, (WPARAM)fUi, 0);
}
static HWND mk(const char *cls, const char *txt, DWORD st, int x, int y, int w, int h, int id) {
    return CreateWindowA(cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, w, h, hMain, (HMENU)(INT_PTR)id, NULL, NULL);
}
static void setup_dialog(void) {
    char b[MAX_PATH] = "";
    if (MessageBoxA(hMain, "Choose any file inside your MSCC folder (for example CHIPS.EXE).", "Setup: game folder", MB_OKCANCEL) != IDOK) return;
    sprintf(b, "%s\\CHIPS.EXE", gameDir);
    if (ask_file(hMain, b, "CHIPS.EXE\0CHIPS.EXE\0All files\0*.*\0", 0)) { char *s = strrchr(b, '\\'); if (s) *s = 0; strcpy(gameDir, b); }
    if (MessageBoxA(hMain, "On 64-bit Windows the game needs a 16-bit launcher such as otvdm.exe.\nChoose it now? (Not needed under Wine or on 32-bit Windows.)", "Setup: 16-bit launcher", MB_YESNO) == IDYES) {
        strcpy(b, vdmCmd); if (ask_file(hMain, b, "Programs\0*.exe\0", 0)) strcpy(vdmCmd, b);
    } else vdmCmd[0] = 0;
    save_settings(); status("Game folder: %s   Launcher: %s", gameDir, vdmCmd[0] ? vdmCmd : "(direct)");
}
static void update_dat_label(void) { char s[MAX_PATH + 32]; sprintf(s, "Level set: %s", datPath[0] ? datPath : "(the game's own CHIPS.DAT)"); SetWindowTextA(hDatLbl, s); }

/* ---- layout: everything is placed from the window size; two splitters (board | panel, data | list) */
static int splitX = -1, dragging; static double dataFrac = 0.5; static int hsplitY, panelTop, panelBottom;
static int text_w(HWND c) {
    char t[256]; GetWindowTextA(c, t, sizeof t); HDC dc = GetDC(c); HFONT o = SelectObject(dc, fUi); SIZE sz;
    GetTextExtentPoint32A(dc, t, (int)strlen(t), &sz); SelectObject(dc, o); ReleaseDC(c, dc); return sz.cx;
}
static int fontH(void) { HDC dc = GetDC(hMain); HFONT o = SelectObject(dc, fUi); TEXTMETRICA tm; GetTextMetricsA(dc, &tm); SelectObject(dc, o); ReleaseDC(hMain, dc); return tm.tmHeight; }
static HWND C(int id) { return GetDlgItem(hMain, id); }
static void place(HWND c, int x, int y, int w, int hh) { if (c) MoveWindow(c, x, y, w, hh, TRUE); }
static void layout(int W, int H) {
    int fh = fontH(), bh = fh + 12, eh = fh + 8, g = 6, x, y = 8;
    int bw(HWND c) { return text_w(c) + 24; }
    /* row 1 */
    x = 8; place(C(ID_SETUP), x, y, bw(C(ID_SETUP)), bh); x += bw(C(ID_SETUP)) + g;
    place(C(ID_DAT), x, y, bw(C(ID_DAT)), bh); x += bw(C(ID_DAT)) + g;
    int sk = bw(C(ID_SAVEKEYS)), rc = text_w(hRecord) + 30;
    place(C(ID_SAVEKEYS), W - 8 - sk, y, sk, bh); place(hRecord, W - 8 - sk - g - rc, y + 2, rc, bh - 4);
    place(hDatLbl, x, y + (bh - fh) / 2, W - 8 - sk - 2 * g - rc - x, fh + 2);
    /* row 2 */
    y += bh + g; x = 8;
    int lw = text_w(C(ID_LBL_LEVEL)) + 6; place(C(ID_LBL_LEVEL), x, y + (bh - fh) / 2, lw, fh + 2); x += lw;
    place(hLevel, x, y + (bh - eh) / 2, 70, eh); x += 70 + 2;
    place(C(ID_LVPREV), x, y, bh + 6, bh); x += bh + 6 + 2; place(C(ID_LVNEXT), x, y, bh + 6, bh); x += bh + 6 + g;
    int ids[] = { ID_GOTO, ID_RESTART, ID_PLAY, ID_STEP, ID_STEP10 };
    for (int i = 0; i < 5; i++) { int ww = bw(C(ids[i])); if (ids[i] == ID_PLAY) { ww = text_w(C(ids[i])) > 0 ? ww : ww; ww += 20; } place(C(ids[i]), x, y, ww, bh); x += ww + g; }
    place(hSpeed, x, y + (bh - eh) / 2, 150, 300); x += 150 + g;
    place(C(ID_OK), x, y, bw(C(ID_OK)), bh);
    /* row 3 */
    y += bh + g; x = 8;
    place(C(ID_LOADIN), x, y, bw(C(ID_LOADIN)), bh); x += bw(C(ID_LOADIN)) + g;
    place(C(ID_RUNIN), x, y, bw(C(ID_RUNIN)), bh); x += bw(C(ID_RUNIN)) + g;
    place(C(ID_SOLPREV), x, y, bw(C(ID_SOLPREV)), bh); x += bw(C(ID_SOLPREV)) + 2;
    place(C(ID_SOLNEXT), x, y, bw(C(ID_SOLNEXT)), bh); x += bw(C(ID_SOLNEXT)) + g;
    int hx = bw(C(ID_HEX)), sl = text_w(C(ID_LBL_SEED)) + 6;
    place(C(ID_HEX), W - 8 - hx, y, hx, bh);
    place(hSeed, W - 8 - hx - g - 120, y + (bh - eh) / 2, 120, eh);
    place(C(ID_LBL_SEED), W - 8 - hx - 2 * g - 120 - sl, y + (bh - fh) / 2, sl, fh + 2);
    place(hInLbl, x, y + (bh - fh) / 2, W - 8 - hx - 3 * g - 120 - sl - x, fh + 2);
    /* status bar */
    place(hStatus, 8, H - 8 - eh, W - 16, eh);
    int top = y + bh + 10, bottom = H - 8 - eh - 8; panelTop = top; panelBottom = bottom;
    /* left: board */
    int under = 2 * (bh + 4) + 4;
    if (splitX < 0) splitX = (int)(W * 0.52);
    if (splitX < 300) splitX = 300; if (splitX > W - 380) splitX = W - 380;
    int cell = (splitX - 8 - 2) / 32, cv = (bottom - top - under - 2) / 32; if (cv < cell) cell = cv; if (cell < 10) cell = 10;
    if (cell != CELL || !fGrid) {
        CELL = cell; if (fGrid) DeleteObject(fGrid);
        fGrid = CreateFontA(-(cell * 65 / 100 < 8 ? 8 : cell * 65 / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, FIXED_PITCH | FF_MODERN, "Courier New");
    }
    int gw = 32 * CELL + 2; place(hGrid, 8, top, gw, gw);
    int yy = top + gw + 6;
    int rt = text_w(C(ID_LAYER_TOP)) + 30, rb = text_w(C(ID_LAYER_BOT)) + 30;
    place(C(ID_LAYER_TOP), 8, yy, rt, bh); place(C(ID_LAYER_BOT), 8 + rt + g, yy, rb, bh);
    yy += bh + 4; int wr = bw(C(ID_CELLW));
    place(C(ID_CELLW), splitX - wr, yy, wr, bh); place(hCellV, splitX - wr - g - 50, yy + (bh - eh) / 2, 50, eh);
    place(hCellLbl, 8, yy + (bh - fh) / 2, splitX - wr - 2 * g - 50 - 8, fh + 2);
    /* right panel */
    int X = splitX + 12, RW = W - 8 - X; y = top;
    place(hInfo, X, y, RW, 6 * (fh + 1) + 6); y += 6 * (fh + 1) + 12;
    int labW = 0; for (int i = 0; i < 10; i++) { int t = text_w(C(ID_LBL_INV0 + i)); if (t > labW) labW = t; }
    labW += 8; int ba = bw(C(ID_INVAPPLY)), br = bw(C(ID_INVREAD));
    int cols = RW >= 3 * (labW + 70 + 2 * g) ? 3 : 2, colW = RW / cols, ew = colW - labW - 2 * g; if (ew > 90) ew = 90; if (ew < 50) ew = 50;
    int rows = cols == 3 ? 4 : 5;
    for (int i = 0; i < 10; i++) {
        int col, row;
        if (cols == 3) { col = i < 4 ? 0 : i < 8 ? 1 : 2; row = i < 4 ? i : i < 8 ? i - 4 : i - 8; }
        else { col = i < 4 ? 0 : i < 8 ? 1 : i - 8; row = i < 4 ? i : i < 8 ? i - 4 : 4; }
        int cx = X + col * colW, cy = y + row * (eh + 4);
        place(C(ID_LBL_INV0 + i), cx, cy + 4, labW, fh + 2); place(hInv[i], cx + labW, cy, ew, eh);
    }
    if (cols == 3) { place(C(ID_INVAPPLY), X + 2 * colW, y + 2 * (eh + 4), ba, eh + 2); place(C(ID_INVREAD), X + 2 * colW, y + 3 * (eh + 4), br, eh + 2); }
    else { place(C(ID_INVAPPLY), W - 8 - ba - g - br, y + rows * (eh + 4), ba, eh + 2); place(C(ID_INVREAD), W - 8 - br, y + rows * (eh + 4), br, eh + 2); rows++; }
    y += rows * (eh + 4) + 6;
    int fl = text_w(C(ID_LBL_FIELD)) + 8, fs = bw(C(ID_FIELDSET));
    place(C(ID_LBL_FIELD), X, y + (bh - fh) / 2, fl, fh + 2);
    place(C(ID_FIELDSET), W - 8 - fs, y, fs, bh); place(hFieldV, W - 8 - fs - g - 110, y + (bh - eh) / 2, 110, eh);
    place(hField, X + fl, y + (bh - eh) / 2, W - 8 - fs - 2 * g - 110 - X - fl, 400);
    y += bh + 8;
    /* data box | splitter | list selector + list */
    int rest = bottom - y - (eh + 4) - 8; if (rest < 80) rest = 80;
    if (dataFrac < 0.15) dataFrac = 0.15; if (dataFrac > 0.85) dataFrac = 0.85;
    int dh = (int)(rest * dataFrac);
    place(hData, X, y, RW, dh); y += dh; hsplitY = y; y += 8;
    place(hListSel, X, y, 240, 300); y += eh + 4;
    place(hList, X, y, RW, bottom - y);
    InvalidateRect(hMain, NULL, TRUE);
}
static void save_layout(void) {
    char b[32]; RECT r; GetWindowRect(hMain, &r);
    sprintf(b, "%d", splitX); WritePrivateProfileStringA("mspptool", "SplitX", b, iniPath);
    sprintf(b, "%d", (int)(dataFrac * 1000)); WritePrivateProfileStringA("mspptool", "DataSplit", b, iniPath);
    if (!IsZoomed(hMain) && !IsIconic(hMain)) { sprintf(b, "%ld %ld %ld %ld", r.left, r.top, r.right - r.left, r.bottom - r.top); WritePrivateProfileStringA("mspptool", "Window", b, iniPath); }
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    char s[256], err[256];
    switch (m) {
    case WM_CREATE: {
        hMain = h;
        mk("BUTTON", "Setup...", 0, 8, 8, 80, 26, ID_SETUP);
        mk("BUTTON", "Level set...", 0, 94, 8, 90, 26, ID_DAT);
        hDatLbl = mk("STATIC", "", SS_PATHELLIPSIS, 190, 13, 470, 20, 0);
        hRecord = mk("BUTTON", "Record data map", BS_AUTOCHECKBOX, 666, 10, 125, 22, ID_RECORD);
        mk("BUTTON", "Save keys as SuperCC JSON...", 0, 796, 8, 200, 26, ID_SAVEKEYS);
        mk("STATIC", "Level:", 0, 8, 46, 40, 20, ID_LBL_LEVEL);
        hLevel = mk("EDIT", "1", WS_BORDER | ES_NUMBER, 50, 43, 50, 24, ID_LEVEL);
        mk("BUTTON", "Launch / go to level", 0, 106, 42, 150, 26, ID_GOTO);
        mk("BUTTON", "Restart level", 0, 262, 42, 100, 26, ID_RESTART);
        hPlay = mk("BUTTON", "Play", 0, 368, 42, 70, 26, ID_PLAY);
        mk("BUTTON", "Step 1 tick", 0, 444, 42, 55, 26, ID_STEP);
        mk("BUTTON", "Step 10 ticks", 0, 505, 42, 65, 26, ID_STEP10);
        hSpeed = mk("COMBOBOX", "", CBS_DROPDOWNLIST, 576, 43, 110, 200, ID_SPEED);
        const char *sp[] = { "x0.25", "x0.5", "x1 (110 ms)", "x2", "x4", "max", "1 move per key" };
        for (int i = 0; i < 7; i++) SendMessageA(hSpeed, CB_ADDSTRING, 0, (LPARAM)sp[i]);
        SendMessageA(hSpeed, CB_SETCURSEL, 2, 0);
        mk("BUTTON", "OK on dialog", 0, 692, 42, 100, 26, ID_OK);
        mk("BUTTON", "Load input...", 0, 8, 76, 100, 26, ID_LOADIN);
        mk("BUTTON", "<", 0, 0, 0, 30, 26, ID_LVPREV); mk("BUTTON", ">", 0, 0, 0, 30, 26, ID_LVNEXT);
        mk("BUTTON", "< Solved", 0, 0, 0, 80, 26, ID_SOLPREV); mk("BUTTON", "Solved >", 0, 0, 0, 80, 26, ID_SOLNEXT);
        mk("BUTTON", "Run input", 0, 114, 76, 80, 26, ID_RUNIN);
        hInLbl = mk("STATIC", "(no input file)", SS_PATHELLIPSIS, 200, 81, 330, 20, 0);
        mk("STATIC", "Seed at start:", 0, 540, 81, 85, 20, ID_LBL_SEED);
        hSeed = mk("EDIT", "0", WS_BORDER, 628, 78, 80, 24, ID_SEED);
        mk("BUTTON", "Hex view...", 0, 714, 76, 78, 26, ID_HEX);
        hGrid = CreateWindowA("MsppGrid", "", WS_CHILD | WS_VISIBLE | WS_BORDER, 8, 112, 32 * CELL + 2, 32 * CELL + 2, h, NULL, NULL, NULL);
        mk("BUTTON", "Top layer", BS_AUTORADIOBUTTON | WS_GROUP, 8, 664, 90, 22, ID_LAYER_TOP);
        mk("BUTTON", "Bottom layer", BS_AUTORADIOBUTTON, 100, 664, 100, 22, ID_LAYER_BOT);
        CheckDlgButton(h, ID_LAYER_TOP, BST_CHECKED);
        hCellLbl = mk("STATIC", "Click a square to edit it.", SS_LEFTNOWORDWRAP, 8, 690, 360, 20, 0);
        hCellV = mk("EDIT", "", WS_BORDER, 370, 688, 40, 22, ID_CELLV);
        mk("BUTTON", "Write", 0, 414, 687, 60, 24, ID_CELLW);
        int X = 568;
        hInfo = mk("STATIC", "", SS_LEFTNOWORDWRAP, X, 112, 430, 92, 0);
        for (int i = 0; i < 10; i++) {
            int col = i < 4 ? 0 : i < 8 ? 1 : 2, row = i < 4 ? i : i < 8 ? i - 4 : i - 8;
            mk("STATIC", INVL[i], 0, X + col * 145, 212 + row * 26, 72, 20, ID_LBL_INV0 + i);
            hInv[i] = mk("EDIT", "", WS_BORDER | ES_NUMBER, X + col * 145 + 74, 209 + row * 26, 60, 22, ID_INV0 + i);
        }
        mk("BUTTON", "Apply inventory", 0, X + 290, 264, 120, 26, ID_INVAPPLY);
        mk("BUTTON", "Read", 0, X + 290, 294, 60, 24, ID_INVREAD);
        mk("STATIC", "Any field:", 0, X, 326, 60, 20, ID_LBL_FIELD);
        hField = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, X + 62, 323, 200, 300, ID_FIELD);
        for (int i = 0; i < NF; i++) SendMessageA(hField, CB_ADDSTRING, 0, (LPARAM)F[i].name);
        SendMessageA(hField, CB_SETCURSEL, 0, 0);
        hFieldV = mk("EDIT", "", WS_BORDER, X + 268, 323, 80, 22, ID_FIELDV);
        mk("BUTTON", "Set", 0, X + 352, 322, 50, 24, ID_FIELDSET);
        hData = mk("EDIT", "", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL | ES_AUTOHSCROLL, X, 352, 430, 200, 0);
        hListSel = mk("COMBOBOX", "", CBS_DROPDOWNLIST, X, 558, 200, 200, ID_LISTSEL);
        const char *ls[] = { "Monster list", "Slide list", "Trap wires", "Clone wires" };
        for (int i = 0; i < 4; i++) SendMessageA(hListSel, CB_ADDSTRING, 0, (LPARAM)ls[i]);
        SendMessageA(hListSel, CB_SETCURSEL, 0, 0);
        hList = mk("LISTBOX", "", WS_BORDER | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT, X, 584, 430, 128, ID_LIST);
        SendMessageA(hList, LB_SETHORIZONTALEXTENT, 700, 0);
        hStatus = mk("STATIC", "", SS_LEFTNOWORDWRAP | WS_BORDER, 8, 718, 990, 22, 0);
        set_fonts(h); SendMessageA(hData, WM_SETFONT, (WPARAM)fMono, 0); SendMessageA(hList, WM_SETFONT, (WPARAM)fMono, 0);
        update_dat_label(); SetTimer(h, ID_REFRESH, 300, NULL);
        { RECT rc; GetClientRect(h, &rc); layout(rc.right, rc.bottom); }
        RegisterHotKey(h, 1, 0, VK_F5); RegisterHotKey(h, 2, 0, VK_F6); RegisterHotKey(h, 3, 0, VK_F7);
        kbHook = SetWindowsHookExA(WH_KEYBOARD_LL, kb_hook, GetModuleHandle(NULL), 0);
        status(gameDir[0] ? "Ready. Game folder: %s" : "Start with Setup: choose your MSCC folder.", gameDir);
        return 0; }
    case WM_TIMER:
        if (w == ID_TIMER && playing) {
            int n = speedMs == 0 ? 10 : 1;
            for (int i = 0; i < n && playing; i++) if (!do_tick()) break;
            refresh(0);
        } else if (w == ID_REFRESH && gProc && !playing) {
            refresh(0);
            if (hHex && IsDlgButtonChecked(hHex, ID_HAUTO)) hex_show();
        }
        if (w == ID_TIMER && hHex && IsDlgButtonChecked(hHex, ID_HAUTO)) hex_show();
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == ID_SETUP) setup_dialog();
        else if (id == ID_DAT) { char b[MAX_PATH]; strcpy(b, datPath); if (ask_file(h, b, "Level sets (*.dat)\0*.dat\0All files\0*.*\0", 0)) {
                int changed = _stricmp(b, datPath) != 0; strcpy(datPath, b); save_settings(); update_dat_label();
                if (changed && inputPath[0]) {      /* a solution file belongs to one level set: unload it */
                    inputPath[0] = 0; nEv = 0; inTicks = 0; inLevel = 0; inHasSeed = 0; twsCount = 0; memset(twsHas, 0, sizeof twsHas);
                    scriptOn = 0; SetWindowTextA(hSeed, "0"); in_label();
                    status("Level set chosen; the input file was unloaded (it belongs to the previous set). Press Launch / go to level.");
                } else status("Level set chosen. Press Launch / go to level."); } }
        else if (id == ID_GOTO) { GetWindowTextA(hLevel, s, sizeof s); if (launch_level(atoi(s) > 0 ? atoi(s) : 1) && perKey) { live_begin(); game_front(); } }
        else if (id == ID_RESTART) { if (gProc) { rec_close(); game_restart(); scriptOn = 0; endedBox = 0; begun = 0; mark_attempt(); set_play(0); memset(keyAt, 0, sizeof keyAt); keyMax = 0; status("Level restarted."); if (perKey) live_begin(); refresh(1); } }
        else if (id == ID_PLAY && perKey && !scriptOn) { status("Speed is \"1 move per key\": press an arrow key (or Space to wait) and the game runs 2 ticks. Choose another speed to play in real time."); game_front(); }
        else if (id == ID_PLAY) { if (gProc) { set_play(!playing); if (playing && tickNo == 0 && !scriptOn) live_begin(); if (playing) game_front(); } }
        else if (id == ID_STEP || id == ID_STEP10) { if (gProc) { set_play(0); if (tickNo == 0 && !scriptOn) live_begin(); for (int i = 0; i < (id == ID_STEP ? 1 : 10); i++) if (!do_tick()) break; refresh(1); game_front();
            if (!scriptOn) status("Tick %d. Ticks run with no key: Chip only moves when you press an arrow key (in the game window, or here) before stepping.", tickNo);
            else status("Tick %d of the input file.", tickNo); } }
        else if (id == ID_LVPREV || id == ID_LVNEXT || id == ID_SOLPREV || id == ID_SOLNEXT) {
            /* previous / next level, or the previous / next level that has a solution in the loaded .tws */
            GetWindowTextA(hLevel, s, sizeof s); int L = atoi(s) > 0 ? atoi(s) : 1, d = (id == ID_LVPREV || id == ID_SOLPREV) ? -1 : 1;
            int maxL = nLevels > 0 ? nLevels : MAXLEV;
            if (id == ID_LVPREV || id == ID_LVNEXT) L += d;
            else {
                if (!inputPath[0] || !strstr(inKind, "TWS")) { status("Load a .tws solution file first (Load input...)."); return 0; }
                int k = L + d; while (k >= 1 && k <= MAXLEV && !twsHas[k]) k += d;
                if (k < 1 || k > MAXLEV) { status("No %s level with a solution in this file.", d < 0 ? "earlier" : "later"); return 0; }
                L = k;
            }
            if (L < 1 || L > maxL) { status("No level %d in this level set (%d levels).", L, maxL); return 0; }
            sprintf(s, "%d", L); SetWindowTextA(hLevel, s);
            if (launch_level(L)) { if (perKey) { live_begin(); game_front(); }
                if (id == ID_SOLPREV || id == ID_SOLNEXT) status("Level %d has a solution: Run input plays it.", L); }
        }
        else if (id == ID_SAVEKEYS) {
            if (!keyMax) { status("No keys played through the tool yet (click the tool window, then use the arrow keys)."); return 0; }
            char b[MAX_PATH] = "solution.json"; OPENFILENAMEA ofn; memset(&ofn, 0, sizeof ofn); ofn.lStructSize = sizeof ofn; ofn.hwndOwner = h;
            ofn.lpstrFilter = "SuperCC solution (*.json)\0*.json\0"; ofn.lpstrFile = b; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR; ofn.lpstrDefExt = "json";
            if (GetSaveFileNameA(&ofn)) {
                FILE *f = fopen(b, "w");
                if (f) { int n = tickNo > keyMax ? tickNo : keyMax + 1;
                    fputs("{\"Moves\":\"", f); for (int i = 0; i < n; i++) fputc(keyAt[i] ? keyAt[i] : '-', f);
                    fprintf(f, "\",\"Seed\":\"%lu\",\"Step\":\"EVEN\",\"Rule\":\"MS\",\"Encode\":\"UTF-8\",\"Initial Slide\":\"UP\"}\n", attemptSeed);
                    fclose(f); status("Saved %d ticks of keys to %s (random state at the start of this attempt: %lu)", n, b, attemptSeed); }
            }
        }
        else if (id == ID_SPEED && HIWORD(w) == CBN_SELCHANGE) {
            static const int ms[] = { 440, 220, 110, 55, 27, 0, 0 }; int sel = (int)SendMessageA(hSpeed, CB_GETCURSEL, 0, 0); speedMs = ms[sel];
            perKey = sel == 6;
            if (perKey) { set_play(0); if (gProc && tickNo == 0 && !scriptOn) live_begin(); status("1 move per key: the game runs 2 ticks (one Chip move) after each arrow key, Space in the game window = wait 2 ticks."); game_front(); }
            else if (playing) { KillTimer(h, ID_TIMER); SetTimer(h, ID_TIMER, speedMs ? speedMs : 10, NULL); }
        }
        else if (id == ID_OK) { game_ok(); status("Pressed OK on the message box (if one was showing)."); refresh(1); game_front(); }
        else if (id == ID_LOADIN) {
            char b[MAX_PATH]; strcpy(b, inputPath);
            GetWindowTextA(hLevel, s, sizeof s); wantLevel = atoi(s) > 0 ? atoi(s) : 1;
            if (ask_file(h, b, "Inputs (*.txt;*.json;*.tws)\0*.txt;*.json;*.tws\0All files\0*.*\0", 0)) {
                if (load_input(b, err)) { strcpy(inputPath, b);
                    if (inLevel) { sprintf(s, "%d", inLevel); SetWindowTextA(hLevel, s); }
                    in_label();
                    if (inHasSeed) { sprintf(s, "%lu", inSeed); SetWindowTextA(hSeed, s); }
                    status("Input loaded. Press Run input (it relaunches the level, then use Play / Step)."); }
                else status("%s", err);
            }
        }
        else if (id == ID_RUNIN) {
            GetWindowTextA(hLevel, s, sizeof s);
            if (inputPath[0] && strstr(inKind, "TWS")) {          /* a .tws holds many levels: use the one in the Level box */
                int L = atoi(s);
                if (L <= 0 || L > MAXLEV || !twsHas[L]) { in_label(); status("This .tws file has no solution for level %d. Type another level number.", L); return 0; }
                if (L != inLevel) { wantLevel = L; load_input(inputPath, err); }
                in_label();
                if (inHasSeed) { sprintf(s, "%lu", inSeed); SetWindowTextA(hSeed, s); }
            }
            if (!nEv && !inTicks) { status("Load an input file first."); return 0; }
            GetWindowTextA(hLevel, s, sizeof s);
            if (launch_level(inLevel ? inLevel : (atoi(s) > 0 ? atoi(s) : 1)) && gProc) {
                if (!strcmp(inKind, "TWS")) { GetWindowTextA(hLevel, s, sizeof s); }
                script_start(); scriptOn = 1; scriptPos = 0;
                if (IsDlgButtonChecked(h, ID_RECORD)) {
                    char p[MAX_PATH], base[MAX_PATH]; const char *nm = strrchr(inputPath, '\\'); strcpy(base, nm ? nm + 1 : inputPath);
                    char *dot = strrchr(base, '.'); if (dot) *dot = 0;
                    sprintf(p, "%s\\records", toolDir); CreateDirectoryA(p, NULL);
                    sprintf(p, "%s\\records\\%s_level%d.jsonl", toolDir, base, inLevel ? inLevel : 1); rec_open(p, inputPath);
                    status("Playing the input file (Pause / Step to stop it). Recording the data map to %s", p);
                } else status("Playing the input file (Pause / Step to stop it).");
                refresh(1); set_play(1); }
        }
        else if (id == ID_HEX) open_hex();
        else if (id == ID_LAYER_TOP || id == ID_LAYER_BOT) { layerBot = id == ID_LAYER_BOT; InvalidateRect(hGrid, NULL, FALSE); }
        else if (id == ID_CELLW) {
            if (selX < 0 || !gProc) { status("Click a square on the board first."); return 0; }
            GetWindowTextA(hCellV, s, sizeof s); unsigned char v = (unsigned char)strtoul(s, NULL, 16);
            if (mem_write(0xFFFF, B + (layerBot ? 0x400 : 0) + selY * 32 + selX, &v, 1, err)) status("Square (%d,%d) %s layer = %02X", selX, selY, layerBot ? "bottom" : "top", v); else status("%s", err);
            refresh(1);
        }
        else if (id == ID_INVAPPLY) {
            if (!gProc) return 0;
            for (int i = 0; i < 10; i++) { GetWindowTextA(hInv[i], s, sizeof s); if (s[0]) fset(fidx(INV[i]), strtoul(s, NULL, 10), NULL); }
            status("Inventory written."); refresh(1);
        }
        else if (id == ID_INVREAD) refresh(1);
        else if (id == ID_LEVEL && HIWORD(w) == EN_CHANGE) in_label();
        else if (id >= ID_INV0 && id < ID_INV0 + 10 && HIWORD(w) == EN_CHANGE) { if (!invSetting) invDirty[id - ID_INV0] = 1; }
        else if (id == ID_FIELD && HIWORD(w) == CBN_SELCHANGE) { int i = (int)SendMessageA(hField, CB_GETCURSEL, 0, 0); sprintf(s, "%lu", fget(i)); SetWindowTextA(hFieldV, s); status("%s: %s", F[i].name, F[i].desc); }
        else if (id == ID_FIELDSET) {
            int i = (int)SendMessageA(hField, CB_GETCURSEL, 0, 0); GetWindowTextA(hFieldV, s, sizeof s);
            if (fset(i, strtoul(s, NULL, 0), err)) status("%s = %s", F[i].name, s); else status("%s", err); refresh(1);
        }
        else if (id == ID_LISTSEL && HIWORD(w) == CBN_SELCHANGE) fill_list();
        return 0; }
    case WM_APP + 5:              /* "1 move per key": a key went to the game, now run one Chip move (2 ticks) */
        if (!gProc || !perKey || scriptOn) return 0;
        if (tickNo == 0) live_begin();
        Sleep(40); for (int i = 0; i < 2; i++) if (!do_tick()) break; refresh(1);
        return 0;
    case WM_HOTKEY:
        if (!gProc) return 0;
        if (w == 1) { SendMessageA(h, WM_COMMAND, ID_PLAY, 0); }
        else if (w == 2 || w == 3) { set_play(0); if (tickNo == 0 && !scriptOn) live_begin(); for (int i = 0; i < (w == 2 ? 1 : 10); i++) if (!do_tick()) break; refresh(1); }
        return 0;
    case WM_SIZE: if (w != SIZE_MINIMIZED && hStatus) layout(LOWORD(l), HIWORD(l)); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO *mm = (MINMAXINFO *)l; mm->ptMinTrackSize.x = 1100; mm->ptMinTrackSize.y = 780; return 0; }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && (HWND)w == h) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            if (p.y > panelTop && p.y < panelBottom && p.x >= splitX && p.x < splitX + 12) { SetCursor(LoadCursor(NULL, IDC_SIZEWE)); return TRUE; }
            if (p.x > splitX + 12 && p.y >= hsplitY && p.y < hsplitY + 8) { SetCursor(LoadCursor(NULL, IDC_SIZENS)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        if (y > panelTop && y < panelBottom && x >= splitX && x < splitX + 12) { dragging = 1; SetCapture(h); }
        else if (x > splitX + 12 && y >= hsplitY && y < hsplitY + 8) { dragging = 2; SetCapture(h); }
        return 0; }
    case WM_MOUSEMOVE:
        if (dragging) {
            RECT rc; GetClientRect(h, &rc); int x = (short)LOWORD(l), y = (short)HIWORD(l);
            if (dragging == 1) splitX = x - 6;
            else { int top = hsplitY - (int)((double)0 + 0); (void)top;
                   RECT d; GetWindowRect(hData, &d); POINT p = { d.left, d.top }; ScreenToClient(h, &p);
                   RECT ls; GetWindowRect(hList, &ls); POINT q = { ls.left, ls.bottom }; ScreenToClient(h, &q);
                   int span = q.y - p.y; if (span > 40) dataFrac = (double)(y - p.y) / span; }
            layout(rc.right, rc.bottom);
        }
        return 0;
    case WM_LBUTTONUP: if (dragging) { dragging = 0; ReleaseCapture(); save_layout(); } return 0;
    case WM_CLOSE: save_layout(); DestroyWindow(h); return 0;
    case WM_DESTROY: UnregisterHotKey(h, 1); UnregisterHotKey(h, 2); UnregisterHotKey(h, 3); if (kbHook) UnhookWindowsHookEx(kbHook);
        rec_close(); game_kill(); ini_restore(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    GetModuleFileNameA(NULL, toolDir, MAX_PATH); char *sl = strrchr(toolDir, '\\'); if (sl) *sl = 0;
    sprintf(iniPath, "%s\\mspptool.ini", toolDir);
    if (__argc >= 6 && !_stricmp(__argv[1], "/test")) { testMode = 1; return run_test(__argc, __argv); }
    load_settings();
    fMono = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, FIXED_PITCH | FF_MODERN, "Courier New");
    fSmall = CreateFontA(-9, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, FIXED_PITCH | FF_MODERN, "Courier New");
    fUi = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, 0, 0, "Tahoma");
    WNDCLASSA wc; memset(&wc, 0, sizeof wc); wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = main_proc; wc.lpszClassName = "MsppMain"; RegisterClassA(&wc);
    wc.lpfnWndProc = grid_proc; wc.lpszClassName = "MsppGrid"; wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH); RegisterClassA(&wc);
    wc.lpfnWndProc = hex_proc; wc.lpszClassName = "MsppHex"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); RegisterClassA(&wc);
    char wb[64]; int wx = 20, wy = 20, ww = 1240, wh = 860;
    GetPrivateProfileStringA("mspptool", "Window", "", wb, sizeof wb, iniPath); sscanf(wb, "%d %d %d %d", &wx, &wy, &ww, &wh);
    splitX = GetPrivateProfileIntA("mspptool", "SplitX", -1, iniPath);
    dataFrac = GetPrivateProfileIntA("mspptool", "DataSplit", 500, iniPath) / 1000.0;
    CreateWindowA("MsppMain", "MSPP tool - MSCC data viewer and stepper", WS_OVERLAPPEDWINDOW | WS_VISIBLE, wx, wy, ww, wh, NULL, NULL, hi, NULL);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && gGame && (msg.wParam == VK_UP || msg.wParam == VK_DOWN || msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT)) {
            char c[32]; GetClassNameA(GetFocus(), c, sizeof c);
            if (_stricmp(c, "Edit") && _stricmp(c, "ComboBox") && _stricmp(c, "ComboLBox") && _stricmp(c, "ListBox") && (msg.hwnd == hMain || GetAncestor(msg.hwnd, GA_ROOT) == hMain)) {   /* arrows in the tool window go to the game */
                if (tickNo == 0 && !scriptOn) live_begin();
                game_front(); Sleep(60);                 /* MSCC takes keys only while its window is active */
                log_key((int)msg.wParam); key_post((int)msg.wParam);
                if (perKey) PostMessageA(hMain, WM_APP + 5, msg.wParam, 0);
                continue;
            }
        }
        if ((msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) && gGame && (msg.wParam == VK_UP || msg.wParam == VK_DOWN || msg.wParam == VK_LEFT || msg.wParam == VK_RIGHT)
            && (msg.hwnd == hMain || GetAncestor(msg.hwnd, GA_ROOT) == hMain)) continue;
        if (hHex && IsDialogMessageA(hHex, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageA(&msg);
    }
    return 0;
}
