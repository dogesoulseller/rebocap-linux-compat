/* windows.h must come before setupapi.h. The blank line keeps clang-format from reordering them. */
#include <windows.h>

#include <setupapi.h>
#include <stdio.h>

int main(void) {
    GUID g[8];
    DWORD n = 0;
    BOOL ok = SetupDiClassGuidsFromNameW(L"Ports", g, 8, &n);
    printf("ClassGuidsFromName ok=%d n=%lu err=%lu\n", ok, n, GetLastError());
    GUID ports = {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    HDEVINFO h = SetupDiGetClassDevsW(&ports, NULL, NULL, DIGCF_PRESENT);
    printf("GetClassDevs h=%p\n", h);
    SP_DEVINFO_DATA d = {sizeof d};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &d); i++) {
        WCHAR id[256] = L"", fn[256] = L"", port[64] = L"";
        DWORD sz = sizeof port;
        SetupDiGetDeviceInstanceIdW(h, &d, id, 256, NULL);
        SetupDiGetDeviceRegistryPropertyW(h, &d, SPDRP_FRIENDLYNAME, NULL, (BYTE *)fn, sizeof fn, NULL);
        HKEY k = SetupDiOpenDevRegKey(h, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        LONG r = -1;
        if (k != INVALID_HANDLE_VALUE) {
            r = RegQueryValueExW(k, L"PortName", NULL, NULL, (BYTE *)port, &sz);
            RegCloseKey(k);
        }
        printf("dev %lu id=%ls fn=%ls regkey=%p r=%ld port=%ls\n", i, id, fn, (void *)k, r, port);
    }
    HANDLE c = CreateFileW(L"\\\\.\\COM5", GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    printf("open COM5 -> %p err=%lu\n", c, GetLastError());
    return 0;
}
