#define UNICODE
#include <stdio.h>
#include <windows.h>
#include <string>
#include <shlwapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <atlcomcli.h>


enum arch
{
    notfound,
    invalid,
    x32,
    x64
};

static bool FileExists(const TCHAR* file)
{
    DWORD attrib = GetFileAttributes(file);
    return (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));
}

static arch GetFileArchitecture(const TCHAR* szFileName)
{
    arch retval = notfound;
    HANDLE hFile = CreateFile(szFileName, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        unsigned char data[0x1000];
        DWORD read = 0;
        DWORD fileSize = GetFileSize(hFile, 0);
        DWORD readSize = sizeof(data);
        if (readSize > fileSize)
            readSize = fileSize;
        if (ReadFile(hFile, data, readSize, &read, 0))
        {
            retval = invalid;
            IMAGE_DOS_HEADER* pdh = (IMAGE_DOS_HEADER*)data;
            if (pdh->e_magic == IMAGE_DOS_SIGNATURE && (size_t)pdh->e_lfanew < readSize)
            {
                IMAGE_NT_HEADERS* pnth = (IMAGE_NT_HEADERS*)(data + pdh->e_lfanew);
                if (pnth->Signature == IMAGE_NT_SIGNATURE)
                {
                    if (pnth->FileHeader.Machine == IMAGE_FILE_MACHINE_I386) //x32
                        retval = x32;
                    else if (pnth->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64) //x64
                        retval = x64;
                }
            }
        }
        CloseHandle(hFile);
    }
    return retval;
}

static bool BrowseFileOpen(HWND owner, const TCHAR* filter, const TCHAR* defext, TCHAR* filename, int filename_size, const TCHAR* init_dir)
{
    OPENFILENAME ofstruct;
    memset(&ofstruct, 0, sizeof(ofstruct));
    ofstruct.lStructSize = sizeof(ofstruct);
    ofstruct.hwndOwner = owner;
    ofstruct.hInstance = GetModuleHandleW(0);
    ofstruct.lpstrFilter = filter;
    ofstruct.lpstrFile = filename;
    ofstruct.nMaxFile = filename_size - 1;
    ofstruct.lpstrInitialDir = init_dir;
    ofstruct.lpstrDefExt = defext;
    ofstruct.Flags = OFN_EXTENSIONDIFFERENT | OFN_HIDEREADONLY | OFN_NONETWORKBUTTON;
    return !!GetOpenFileName(&ofstruct);
}


#define SHELLEXT_EXE_KEY TEXT("exefile\\shell\\Debug with x64dbg\\Command")
#define SHELLEXT_DLL_KEY TEXT("dllfile\\shell\\Debug with x64dbg\\Command")

static TCHAR* GetDesktopPath()
{
    static TCHAR path[MAX_PATH + 1];
    if (SHGetSpecialFolderPath(HWND_DESKTOP, path, CSIDL_DESKTOPDIRECTORY, FALSE))
        return path;
    else
        return NULL;
}

static HRESULT AddDesktopShortcut(TCHAR* szPathOfFile, const TCHAR* szNameOfLink)
{
    HRESULT hRes = NULL;

    //Get the working directory
    TCHAR pathFile[MAX_PATH + 1];
    _tcscpy(pathFile, szPathOfFile);
    PathRemoveFileSpec(pathFile);

    CComPtr<IShellLink> psl;
    hRes = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLink, (LPVOID*)&psl);
    if (SUCCEEDED(hRes))
    {
        CComPtr<IPersistFile> ppf;

        psl->SetPath(szPathOfFile);
        psl->SetDescription(TEXT("A Debugger for the future!"));
        psl->SetIconLocation(szPathOfFile, 0);
        psl->SetWorkingDirectory(pathFile);

        hRes = psl->QueryInterface(IID_IPersistFile, (LPVOID*)&ppf);
        if (SUCCEEDED(hRes))
        {
            TCHAR path[MAX_PATH + 1] = TEXT("");
            _tmakepath(path, NULL, GetDesktopPath(), szNameOfLink, TEXT("lnk"));
            CComBSTR tmp(path);
            hRes = ppf->Save(tmp, TRUE);
        }
    }
    return hRes;
}

static void RegisterShellExtension(const TCHAR* key, const TCHAR* command)
{
    HKEY hKey;
    if (RegCreateKey(HKEY_CLASSES_ROOT, key, &hKey) != ERROR_SUCCESS)
    {
        MessageBox(0, TEXT("RegCreateKeyA failed!"), TEXT("Running as Admin?"), MB_ICONERROR);
        return;
    }
    if (RegSetValueEx(hKey, 0, 0, REG_EXPAND_SZ, (LPBYTE)command, (_tcslen(command) + 1) * sizeof(TCHAR)) != ERROR_SUCCESS)
        MessageBox(0, TEXT("RegSetValueExA failed!"), TEXT("Running as Admin?"), MB_ICONERROR);
    RegCloseKey(hKey);
}

static void CreateUnicodeFile(const TCHAR* file)
{
    //Taken from: http://www.codeproject.com/Articles/9071/Using-Unicode-in-INI-files
    if (FileExists(file))
        return;

    // UTF16-LE BOM(FFFE)
    WORD wBOM = 0xFEFF;
    HANDLE hFile = CreateFile(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(hFile, &wBOM, sizeof(WORD), &written, NULL);
    CloseHandle(hFile);
}

//Taken from: http://www.cplusplus.com/forum/windows/64088/
static bool ResolveShortcut(HWND hwnd, const TCHAR* szShortcutPath, TCHAR* szResolvedPath, size_t nSize)
{
    if (szResolvedPath == NULL)
        return SUCCEEDED(E_INVALIDARG);

    //Get a pointer to the IShellLink interface.
    CComPtr<IShellLink> psl;
    HRESULT hres = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLink, (LPVOID*)&psl);
    if (SUCCEEDED(hres))
    {
        //Get a pointer to the IPersistFile interface.
        CComPtr<IPersistFile> ppf;
        hres = psl->QueryInterface(IID_IPersistFile, (void**)&ppf);
        if (SUCCEEDED(hres))
        {
            //Load the shortcut.
            CComBSTR tmp(szShortcutPath);
            hres = ppf->Load(tmp, STGM_READ);

            if (SUCCEEDED(hres))
            {
                //Resolve the link.
                hres = psl->Resolve(hwnd, 0);

                if (SUCCEEDED(hres))
                {
                    //Get the path to the link target.
                    TCHAR szGotPath[MAX_PATH] = { 0 };
                    hres = psl->GetPath(szGotPath, _countof(szGotPath), NULL, SLGP_SHORTPATH);

                    if (SUCCEEDED(hres))
                    {
                        _tcscpy_s(szResolvedPath, nSize, szGotPath);
                    }
                }
            }
        }
    }
    return SUCCEEDED(hres);
}

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    CoInitialize(NULL); //fixed some crash
    //Get INI file path
    TCHAR szModulePath[MAX_PATH] = TEXT("");
    if (!GetModuleFileName(0, szModulePath, MAX_PATH))
    {
        MessageBox(0, TEXT("Error getting module path!"), TEXT("Error"), MB_ICONERROR | MB_SYSTEMMODAL);
        return 0;
    }
    TCHAR szIniPath[MAX_PATH] = TEXT("");
    _tcscpy_s(szIniPath, szModulePath);
    TCHAR szCurrentDir[MAX_PATH] = TEXT("");
    _tcscpy_s(szCurrentDir, szModulePath);
    int len = (int)_tcslen(szCurrentDir);
    while (szCurrentDir[len] != TEXT('\\') && len)
        len--;
    if (len)
        szCurrentDir[len] = TEXT('\0');
    len = (int)_tcslen(szIniPath);
    while (szIniPath[len] != TEXT('.') && szIniPath[len] != TEXT('\\') && len)
        len--;
    if (szIniPath[len] == TEXT('\\'))
        _tcscat_s(szIniPath, TEXT(".ini"));
    else
        _tcscpy(&szIniPath[len], TEXT(".ini"));
    CreateUnicodeFile(szIniPath);

    //Load settings
    bool bDoneSomething = false;
    TCHAR sz32Path[MAX_PATH] = TEXT("");
    if (!GetPrivateProfileString(TEXT("Launcher"), TEXT("x32dbg"), TEXT(""), sz32Path, MAX_PATH, szIniPath))
    {
        _tcscpy_s(sz32Path, szCurrentDir);
        PathAppend(sz32Path, TEXT("x32\\x32dbg.exe"));
        if (FileExists(sz32Path))
        {
            WritePrivateProfileString(TEXT("Launcher"), TEXT("x32dbg"), sz32Path, szIniPath);
            bDoneSomething = true;
        }
    }

    TCHAR sz32Dir[MAX_PATH] = TEXT("");
    _tcscpy_s(sz32Dir, sz32Path);
    PathRemoveFileSpec(sz32Dir);

    TCHAR sz64Path[MAX_PATH] = TEXT("");
    if (!GetPrivateProfileString(TEXT("Launcher"), TEXT("x64dbg"), TEXT(""), sz64Path, MAX_PATH, szIniPath))
    {
        _tcscpy_s(sz64Path, szCurrentDir);
        PathAppend(sz64Path, TEXT("x64\\x64dbg.exe"));
        if (FileExists(sz64Path))
        {
            WritePrivateProfileString(TEXT("Launcher"), TEXT("x64dbg"), sz64Path, szIniPath);
            bDoneSomething = true;
        }
    }

    TCHAR sz64Dir[MAX_PATH] = TEXT("");
    _tcscpy_s(sz64Dir, sz64Path);
    PathRemoveFileSpec(sz64Dir);

    //Handle command line
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc <= 1) //no arguments -> set configuration
    {
        if (!FileExists(sz32Path) && BrowseFileOpen(0, TEXT("x32dbg.exe\0x32dbg.exe\0\0"), 0, sz32Path, MAX_PATH, szCurrentDir))
        {
            WritePrivateProfileString(TEXT("Launcher"), TEXT("x32dbg"), sz32Path, szIniPath);
            bDoneSomething = true;
        }
        if (!FileExists(sz64Path) && BrowseFileOpen(0, TEXT("x64dbg.exe\0x64dbg.exe\0\0"), 0, sz64Path, MAX_PATH, szCurrentDir))
        {
            WritePrivateProfileString(TEXT("Launcher"), TEXT("x64dbg"), sz64Path, szIniPath);
            bDoneSomething = true;
        }
        if (MessageBox(0, TEXT("Do you want to register a shell extension?"), TEXT("Question"), MB_YESNO | MB_ICONQUESTION) == IDYES)
        {
            TCHAR szLauncherCommand[MAX_PATH] = TEXT("");
            _stprintf_s(szLauncherCommand, _countof(szLauncherCommand), TEXT("\"%s\" \"%%1\""), szModulePath);
            RegisterShellExtension(SHELLEXT_EXE_KEY, szLauncherCommand);
            RegisterShellExtension(SHELLEXT_DLL_KEY, szLauncherCommand);
        }
        if (MessageBox(0, TEXT("Do you want to create Desktop Shortcuts?"), TEXT("Question"), MB_YESNO | MB_ICONQUESTION) == IDYES)
        {
            AddDesktopShortcut(sz32Path, TEXT("x32dbg"));
            AddDesktopShortcut(sz64Path, TEXT("x64dbg"));
        }
        if (bDoneSomething)
            MessageBox(0, TEXT("New configuration written!"), TEXT("Done!"), MB_ICONINFORMATION);
    }
    if (argc == 2) //one argument -> execute debugger
    {
        TCHAR szPath[MAX_PATH] = TEXT("");
        _tcscpy_s(szPath, argv[1]);
        TCHAR szResolvedPath[MAX_PATH] = TEXT("");

        ResolveShortcut(0, szPath, szResolvedPath, _countof(szResolvedPath));

        //TODO: Use WinAPI here so we can avoid having to compile in Unicode
        std::wstring cmdLine = TEXT("\"");
        cmdLine += szPath;
        cmdLine += L"\"";
        switch (GetFileArchitecture(szPath))
        {
        case x32:
            if (sz32Path[0])
                ShellExecute(0, TEXT("open"), sz32Path, cmdLine.c_str(), sz32Dir, SW_SHOWNORMAL);
            else
                MessageBox(0, TEXT("Path to x32dbg not specified in launcher configuration..."), TEXT("Error!"), MB_ICONERROR);
            break;

        case x64:
            if (sz64Path[0])
                ShellExecute(0, TEXT("open"), sz64Path, cmdLine.c_str(), sz64Dir, SW_SHOWNORMAL);
            else
                MessageBox(0, TEXT("Path to x64dbg not specified in launcher configuration..."), TEXT("Error!"), MB_ICONERROR);
            break;

        case invalid:
            MessageBox(0, argv[1], TEXT("Invalid PE File!"), MB_ICONERROR);
            break;

        case notfound:
            MessageBox(0, argv[1], TEXT("File not found or in use!"), MB_ICONERROR);
            break;
        }
    }
    LocalFree(argv);
    return 0;
}
