/* DATATAP.EXE - Win16 data tap for the dirty-side harness.
 * Launches CHIPS.EXE in this same VDM, then on request writes TAPMAP.TXT listing the
 * game's memory blocks through ToolHelp. Code blocks (GT_CODE) are never listed or read:
 * only their count is reported. The map gives each non-code block's handle, type, size and
 * linear address, plus the game task's stack bounds so the reader can blank the stack.
 * Request: send the registered message "MSPP_TAPMAP" (or WM_USER+1) to the window "DataTap". */
#include <windows.h>
#include <toolhelp.h>
#include <stdio.h>
#include <string.h>

static HINSTANCE chipsInst;
/* access rights of a selector from its descriptor (0 if invalid). bit 0x0800 = executable */
unsigned lar(unsigned sel);
#pragma aux lar = "xor ax,ax" "lar ax,bx" parm [bx] value [ax];
static UINT tapMsg;

static void writemap(void) {
    TASKENTRY te; GLOBALENTRY ge; FILE *f; HTASK task = 0; HMODULE mod = 0; int ncode = 0; unsigned sel, ar;
    te.dwSize = sizeof te;
    if (TaskFirst(&te)) do {
        if (!_fstricmp(te.szModule, "CHIPS")) { task = te.hTask; mod = te.hModule; break; }
    } while (TaskNext(&te));
    f = fopen("TAPMAP.TMP", "w"); if (!f) return;
    if (!task) { fprintf(f, "notask\n"); fclose(f); remove("TAPMAP.TXT"); rename("TAPMAP.TMP", "TAPMAP.TXT"); return; }
    fprintf(f, "task %04X module %04X inst %04X ss %04X sp %04X stacktop %04X stackmin %04X stackbot %04X\n",
            (unsigned)task, (unsigned)mod, (unsigned)te.hInst, te.wSS, te.wSP, te.wStackTop, te.wStackMinimum, te.wStackBottom);
    ge.dwSize = sizeof ge;
    if (GlobalFirst(&ge, GLOBAL_ALL)) do {
        if (ge.wType == GT_FREE) continue;
        sel = GlobalHandleToSel(ge.hBlock); ar = lar(sel);
        {
          /* never list code: ToolHelp type CODE, an executable descriptor, or a descriptor we cannot read */
          if (ge.wType == GT_CODE || !ar || (ar & 0x0800)) { ncode++; continue; } }
        fprintf(f, "block %04X owner %04X type %u data %u size %lu addr %08lX lock %u access %04X\n",
                (unsigned)ge.hBlock, (unsigned)ge.hOwner, ge.wType, ge.wData, ge.dwBlockSize, ge.dwAddress, ge.wcLock, ar);
    } while (GlobalNext(&ge, GLOBAL_ALL));
    fprintf(f, "codeblocks %d (code or unreadable descriptor, not listed)\nend\n", ncode);
    fclose(f); remove("TAPMAP.TXT"); rename("TAPMAP.TMP", "TAPMAP.TXT");
}

LRESULT CALLBACK __export WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == tapMsg || m == WM_USER + 1) { writemap(); return 1; }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    WNDCLASS wc; MSG msg; HWND h;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = "DataTapClass";
    RegisterClass(&wc);
    tapMsg = RegisterWindowMessage("MSPP_TAPMAP");
    h = CreateWindow("DataTapClass", "DataTap", WS_OVERLAPPED, 0, 0, 10, 10, NULL, NULL, inst, NULL);
    chipsInst = (HINSTANCE)WinExec(cmd && *cmd ? cmd : "CHIPS.EXE", SW_SHOWNORMAL);
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}
