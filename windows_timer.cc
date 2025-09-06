#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#pragma comment(lib, "winmm.lib") // 注意：库名是 "winmm.lib"
void CALLBACK OnTimer(UINT id, UINT, DWORD_PTR userData, DWORD_PTR, DWORD_PTR) {
    printf("Timer struck at %u\n", ::GetTickCount());
}
int main() {
    auto id = ::timeSetEvent(
        1000, // interval (msec)
        10, // resolution (msec)
        OnTimer, // callback
        0, // user data
        TIME_PERIODIC); // periodic or one shot
    
    ::Sleep(10000);
    ::timeKillEvent(id);
    return 0;
}
//cl /EHsc /W4 /std:c++17 windows_timer.cc /link winmm.lib