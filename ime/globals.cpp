#include "globals.h"

HINSTANCE g_hInst = NULL;
LONG g_cRefDll = 0;

void dll_add_ref()
{
    InterlockedIncrement(&g_cRefDll);
}

void dll_release()
{
    InterlockedDecrement(&g_cRefDll);
}
