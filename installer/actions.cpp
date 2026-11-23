#include <windows.h>
#include <msi.h>
#include <shellapi.h>
#include <shlobj.h>

extern "C" __declspec(dllexport) UINT WINAPI NotifyExplorer(MSIHANDLE) {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    return ERROR_SUCCESS;
}

extern "C" __declspec(dllexport) UINT WINAPI OpenDefaultApps(MSIHANDLE) {
    const auto opened = ShellExecuteW(nullptr, L"open",
        L"ms-settings:defaultapps?registeredAppUser=STL%20Quick%20View",
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32)
        ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
    return ERROR_SUCCESS;
}
