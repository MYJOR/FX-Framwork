#include <windows.h>
#include <stdio.h>
typedef HRESULT(WINAPI *F)(void);
int main(){
  HDC dc=GetDC(NULL); printf("VREFRESH=%d\n", GetDeviceCaps(dc,VREFRESH));
  DEVMODEA dm={0}; dm.dmSize=sizeof dm; EnumDisplaySettingsA(NULL,ENUM_CURRENT_SETTINGS,&dm); printf("devmode hz=%lu\n", dm.dmDisplayFrequency);
  F f=(F)(void*)GetProcAddress(LoadLibraryA("dwmapi.dll"),"DwmFlush");
  LARGE_INTEGER q,a,b; QueryPerformanceFrequency(&q);
  for(int i=0;i<8;i++){ QueryPerformanceCounter(&a); HRESULT h=f(); QueryPerformanceCounter(&b); printf("flush %.2f ms (hr=%lx)\n",(b.QuadPart-a.QuadPart)*1000.0/q.QuadPart,(unsigned long)h);}
  return 0;}
