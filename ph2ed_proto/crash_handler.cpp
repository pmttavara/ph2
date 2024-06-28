#define _CRT_SECURE_NO_WARNINGS

#include "app_details.h"
#define STR_(x) #x
#define STR(x) STR_(x)
#define GIT_HASH_STRING STR(GIT_HASH)

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#ifndef defer
struct defer_dummy {};
template <class F> struct deferrer { F f; ~deferrer() { f(); } };
template <class F> deferrer<F> operator*(defer_dummy, F f) { return {f}; }
#define DEFER_(LINE) zz_defer##LINE
#define DEFER(LINE) DEFER_(LINE)
#define defer auto DEFER(__LINE__) = defer_dummy{} *[&]()
#endif // defer

#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <winternl.h>
#include <DbgHelp.h>
#include <Commctrl.h>
#include <Winhttp.h>
#include <Psapi.h>
#pragma comment(lib, "comctl32")
#pragma comment(lib, "dbghelp")
#pragma comment(lib, "winhttp")

#include "zip.h"

// The Discord webhook URL has been very mildly obfuscated in the source code in order to prevent GitHub scanner bots from spamming the hook.
// More obscurity could be added by XORing some of the bytes at preprocess time and unXORing them at runtime, but I'll do that the next time I get pwned.
#define DISCORD_WEBHOOK_ENDPOINT "/" \
/* ------------------------- */ "api" \
/* ------------------------ */ "/webh" \
/* ----------------------- */ "ooks/12" \
/* ---------------------- */ "560616280" \
/* --------------------- */ "76785675/PB" \
/* -------------------- */ "AO837hCGxWLY4" \
/* ------------------- */ "-x9IGELpcj5aVi_" \
/* ------------------ */ "0-iNXieQA2dicPg66" \
/* ----------------- */ "rUgYBRVaM41L0KVmg20tj"

BOOL CALLBACK minidump_callback(void *userdata, MINIDUMP_CALLBACK_INPUT *input, MINIDUMP_CALLBACK_OUTPUT *output) {

    if (input->CallbackType == ModuleCallback) {
        WCHAR  main[65536];
        DWORD  main_n = GetProcessImageFileNameW(input->ProcessHandle, main, 65536);
        WCHAR *path = input->Module.FullPath;
        DWORD  path_n = 0;
        while (path[path_n]) { // wcslen() can crash ASan
            path_n++;
        }
        if (main_n && path_n) {
            for (int a = main_n - 1, b = path_n - 1; a >= 0 && b >= 0; --a, --b) {
                if (main[a] != path[b]) {
                    output->ModuleWriteFlags &= ~ModuleWriteDataSeg;
                    break;
                }
                if (path[b] == '/' || path[b] == '\\') {
                    break;
                }
            }
        }
        if (output->ModuleWriteFlags & ModuleWriteDataSeg) {
            printf("Dumping data seg for module %ls\n", path);
        }
    }

    return TRUE;
}

void do_crash_handler() {

    // Get the command line
    int argc = 0;
    wchar_t *cmd = GetCommandLineW();
    if (!cmd || !cmd[0]) {
        return; // Error: just run the app without a crash handler.
    }
    wchar_t **wargv = CommandLineToArgvW(cmd, &argc); // Passing nullptr here crashes!
    if (!wargv || !wargv[0]) {
        return; // Error: just run the app without a crash handler.
    }

    // Parse the command line for -no-crash-handler
    bool crash_handler = true;
    for (int i = 0; i < argc; ++i) {
        if (!wcscmp(wargv[i], L"-no-crash-handler")) {
            crash_handler = false;
        }
    }
    if (!crash_handler) { // We already *are* the subprocess - continue with the main program!
        return;
    }

    // Concatenate -no-crash-handler onto the command line for the subprocess
    int cmd_n = 0;
    while (cmd[cmd_n]) { // wcslen() can crash ASan
        cmd_n++;
    }
    const wchar_t *append = L" -no-crash-handler";
    int append_n = 0;
    while (append[append_n]) { // wcslen() can crash ASan
        append_n++;
    }
    wchar_t *cmd_new = (wchar_t *)calloc(cmd_n + append_n + 1, sizeof(wchar_t)); // @Leak
    if (!cmd_new) {
        return; // Error: just run the app without a crash handler.
    }
    memcpy(cmd_new, cmd, cmd_n * sizeof(wchar_t));
    memcpy(cmd_new + cmd_n, append, append_n * sizeof(wchar_t));

    // Parameters for starting the subprocess
    STARTUPINFOW siw = {};
    siw.cb = sizeof(siw);
    siw.dwFlags = STARTF_USESTDHANDLES;
    siw.hStdInput = GetStdHandle(STD_INPUT_HANDLE); // @Leak: CloseHandle()
    siw.hStdOutput = GetStdHandle(STD_ERROR_HANDLE);
    siw.hStdError = GetStdHandle(STD_OUTPUT_HANDLE);
    PROCESS_INFORMATION pi = {}; // @Leak: CloseHandle()

    // Launch suspended, then read-modify-write the PEB (see below), then resume -p 2022-03-04
    if (!CreateProcessW(nullptr, cmd_new, nullptr, nullptr, true,
                        CREATE_SUSPENDED | DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr, &siw, &pi)) {
        // If we couldn't create a subprocess, then just run the program without a crash handler.
        // That's not great, but it's presumably better than stopping the user from running at all!
        return;
    }

    // NOTE: SteamAPI_Init() takes WAY longer On My Machine(tm) when a debugger is present.
    //       (The DLL file steam_api64.dll does indeed call IsDebuggerPresent() sometimes.)
    //       It's clear that Steam does extra niceness for us when debugging, but we DO NOT
    //       want this to destroy our load times; I measure 3.5x slowdown (0.6s -> 2.1s).
    //       The only way I know to trick the child process into thinking it is free of a
    //       debugger is to clear the BeingDebugged byte in the Process Environment Block.
    //       If we are unable to perform this advanced maneuver, we will gracefully step back
    //       and allow Steam to ruin our loading times. -p 2022-03-04
    auto persuade_process_no_debugger_is_present = [] (HANDLE hProcess) {

        // Load NTDLL
        HMODULE ntdll = LoadLibraryA("ntdll.dll");
        if (!ntdll) return;

        // Get NtQueryInformationProcess function
        auto NtQueryInformationProcess =
            (/*__kernel_entry*/ NTSTATUS (*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG))
                GetProcAddress(ntdll, "NtQueryInformationProcess");
        if (!NtQueryInformationProcess) return;

        // Query process information to find the PEB address
        PROCESS_BASIC_INFORMATION pbi;
        DWORD query_bytes_read = 0;
        if (NtQueryInformationProcess(hProcess, ProcessBasicInformation, &pbi, sizeof(pbi), &query_bytes_read) != 0
            || query_bytes_read != sizeof(pbi)) return;

        // Read the PEB of the child process
        PEB peb;
        SIZE_T process_bytes_read = NULL;
        if (!ReadProcessMemory(hProcess, pbi.PebBaseAddress, &peb, sizeof(peb), &process_bytes_read)
            || process_bytes_read != sizeof(peb)) return;

        // Gaslight the child into believing we are not watching
        peb.BeingDebugged = 0;

        // Write back the modified PEB
        SIZE_T process_bytes_written = NULL;
        if (!WriteProcessMemory(hProcess, pbi.PebBaseAddress, &peb, sizeof(peb), &process_bytes_written)
            || process_bytes_written != sizeof(peb)) return;
    };
    persuade_process_no_debugger_is_present(pi.hProcess);

    // Helper function to destroy the subprocess
    auto exit_child = [&] {
        TerminateProcess(pi.hProcess, 1); // Terminate before detaching, so you don't see Windows Error Reporting.
        DebugActiveProcessStop(GetProcessId(pi.hProcess)); // Detach
        WaitForSingleObject(pi.hProcess, 2000); // Wait for child to die, but not forever.
    };

    // Kick off the subprocess
    if (ResumeThread(pi.hThread) != 1) {
        exit_child();
        return;
    }

    // Debugger loop: catch (and ignore) all debug events until the program exits or hits a last-chance exception
    HANDLE file = nullptr;
    DEBUG_EVENT de = {};
    for (;;) {

        // Get debug event
        de = {};
        if (!WaitForDebugEvent(&de, INFINITE)) {
            exit_child();
            ExitProcess(1);
        }

        // If the process exited, nag about failure, or silently exit on success
        if (de.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT && de.dwProcessId == pi.dwProcessId) {

            // If the process exited unsuccessfully, prompt to restart it
            // @Todo: in these cases, no dump can be made, so upload just the stdout log and profiling trace
            if (de.u.ExitThread.dwExitCode != 0) {

                // Terminate & detach just to be safe
                exit_child();
            }

            // Bubble up the failure code - this is where successful program runs will end up!
            ExitProcess(de.u.ExitThread.dwExitCode);
        }

        // If the process had some other debug stuff, we don't care.
        if (de.dwDebugEventCode != EXCEPTION_DEBUG_EVENT) {
            ContinueDebugEvent(de.dwProcessId, de.dwThreadId, DBG_CONTINUE);
            continue;
        }

        // Skip first-chance exceptions or exceptions for processes we don't care about (shouldn't ever happen).
        if (de.u.Exception.dwFirstChance || de.dwProcessId != GetProcessId(pi.hProcess)) {
            ContinueDebugEvent(de.dwProcessId, de.dwThreadId, DBG_EXCEPTION_NOT_HANDLED);
            continue;
        }

        // By here, we have hit a real, last-chance exception. This is a crash we should generate a dump for.
        break;
    }

    // Create crash dump filename
    char filename[sizeof("PH2_CrashDump_18446744073709551616.dmp.zip")];
    memset(filename, 0xcc, sizeof(filename));
    time_t dump_time = time(NULL);
    int filename_n = snprintf(filename, sizeof(filename), "PH2_CrashDump_%llu.dmp", (uint64_t)dump_time);
    if (filename_n <= sizeof("PH2_CrashDump_.dmp") || filename_n >= sizeof(filename) - 4 || filename[filename_n] != 0) {
        exit_child();
        ExitProcess(1);
    }

    // Convert filename to UTF-16
    WCHAR filename16[sizeof(filename)];
    memset(filename16, 0xcc, sizeof(filename16));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename, -1, (LPWSTR)filename16, filename_n + 1) != filename_n + 1 ||
        filename16[filename_n] != 0) {
        exit_child();
        ExitProcess(1);
    }

    // Create crash dump file
    file = CreateFileW((LPCWSTR)filename16, GENERIC_WRITE | GENERIC_READ, 0, nullptr,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        exit_child();
        ExitProcess(1);
    }

    // Generate exception pointers out of excepting thread context
    CONTEXT c = {};
    if (HANDLE thread = OpenThread(THREAD_ALL_ACCESS, true, de.dwThreadId)) {
        c.ContextFlags = CONTEXT_ALL;
        GetThreadContext(thread, &c);
        CloseHandle(thread);
    }
    EXCEPTION_POINTERS ep = {};
    ep.ExceptionRecord = &de.u.Exception.ExceptionRecord;
    ep.ContextRecord = &c;
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = de.dwThreadId;
    mei.ExceptionPointers = &ep;
    mei.ClientPointers = false;

    // You could add some others here, but these should be good.
    int flags = MiniDumpNormal
              | MiniDumpWithDataSegs
              | MiniDumpWithHandleData
              | MiniDumpScanMemory
              | MiniDumpWithUnloadedModules
              | MiniDumpWithIndirectlyReferencedMemory
              | MiniDumpFilterModulePaths
              | MiniDumpWithProcessThreadData
              | MiniDumpWithThreadInfo
              | MiniDumpIgnoreInaccessibleMemory;

    // Write minidump
    MINIDUMP_CALLBACK_INFORMATION mci = { minidump_callback };
    if (!MiniDumpWriteDump(pi.hProcess, GetProcessId(pi.hProcess), file,
                           (MINIDUMP_TYPE)flags, &mei, nullptr, &mci)) {
        exit_child();
        ExitProcess(1);
    }

    // Cleanup: Destroy subprocess now that we have a dump.
    // Note that we want to do this before doing any blocking interface dialogs,
    // because otherwise you would leave an arbitrarily broken program lying around
    // longer than you need to.
    exit_child();

    MessageBoxW(nullptr,
                L"The app had a fatal error and must close.\n"
                "Please report this to Github at:\n"
                "https://github.com/pmttavara/ph2/issues\n",
                L"Fatal Error", MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND);

    // Get crash dump file size
    uint64_t file_size = 0;
    if (!GetFileSizeEx(file, (LARGE_INTEGER *)&file_size) || !file_size) {
        ExitProcess(1);
    }

    {
        char filename_zip[sizeof(filename)];
        WCHAR filename_zip16[sizeof(filename)];
        memset(filename_zip, 0xcc, sizeof(filename_zip));
        memset(filename_zip16, 0xcc, sizeof(filename_zip16));
        int filename_zip_n = filename_n + 4;
        if (filename_zip_n + 1 < sizeof(filename)) {
            memcpy(filename_zip,   filename,   filename_n * 1); memcpy(filename_zip   + filename_n,  ".zip\0", 5 * 1);
            memcpy(filename_zip16, filename16, filename_n * 2); memcpy(filename_zip16 + filename_n, L".zip\0", 5 * 2);
            struct zip_t *zip = zip_open(filename_zip, 9, 'w');
            defer {
                // If we got to the end without clearing out the zipped filename, we failed -- delete the zip file
                if (filename_zip_n) {
                    DeleteFileW((LPCWSTR)filename_zip16);
                }
            };
            if (zip) {
                if (zip_entry_open(zip, filename) == 0) {
                    bool success = true;
                    char buf[8192] = {0};
                    DWORD n = 0;
                    // Seek file to start
                    if (SetFilePointer(file, 0, nullptr, FILE_BEGIN) != 0) {
                        success = false;
                    }
                    if (success) {
                        while (ReadFile(file, buf, sizeof(buf), &n, nullptr) && n > 0) {
                            if (zip_entry_write(zip, buf, n) < 0) {
                                success = false;
                                break;
                            }
                        }
                    }
                    if (zip_entry_close(zip)) {
                        success = false;
                    }
                    zip_close(zip);
                    if (success) {
                        HANDLE file_zip = CreateFileW((LPCWSTR)filename_zip16, GENERIC_READ, 0, nullptr,
                                                                               OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
                        uint64_t file_size_zip = 0;
                        if (file_zip != INVALID_HANDLE_VALUE && GetFileSizeEx(file_zip, (LARGE_INTEGER *)&file_size_zip)) {

                            // The file has been zipped, so we don't need the original anymore -- delete it
                            CloseHandle(file);
                            DeleteFileW((LPCWSTR)filename16);

                            // Swap out the original file for the zipped file
                            memcpy(filename,   filename_zip,   (filename_zip_n + 1) * 1);
                            memcpy(filename16, filename_zip16, (filename_zip_n + 1) * 2);
                            filename_n = filename_zip_n;
                            file = file_zip;
                            file_size = file_size_zip;

                            filename_zip_n = 0;
                            file_zip = INVALID_HANDLE_VALUE;
                        }
                    }
                } else {
                    zip_close(zip);
                }
            }
        }
    }

    // Discord limit is 25 MB -- can't upload if it's bigger than this
    if (file_size >= 25000000) {
        ExitProcess(1);
    }

    // Build MIME multipart-form payload
    static char body[1 << 25];
    const char body_prefix[] =
        "--19024605111143684786787635207\r\n"
        "Content-Disposition: form-data; name=\"payload_json\"\r\n\r\n{\"content\":\""
        "**PH2 Editor Crash Report**\\n- Version: **v" APP_VERSION_STRING "**\\n- Git Hash: `" GIT_HASH_STRING "` (<https://github.com/pmttavara/ph2/commit/" GIT_HASH_STRING ">)"
        "\"}\r\n--19024605111143684786787635207\r\n"
        "Content-Disposition: form-data; name=\"files[0]\"; filename=\"";
    const char body_infix[] = "\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n";
    const char body_postfix[] = "\r\n--19024605111143684786787635207--\r\n";

    // Printf the prefix, filename, infix
    int header_n = snprintf(body, sizeof(body), "%s%s%s", body_prefix, filename, body_infix);
    if (header_n != sizeof(body_prefix) - 1 + filename_n + sizeof(body_infix) - 1) {
        ExitProcess(1);
    }

    int body_n = header_n + file_size + sizeof(body_postfix) - 1;
    if (body_n >= sizeof(body)) { // buffer overflow
        ExitProcess(1);
    }

    // Seek file to start
    if (SetFilePointer(file, 0, nullptr, FILE_BEGIN) != 0) {
        ExitProcess(1);
    }

    // Copy entire file into the space after the body infix
    DWORD bytes_read = 0;
    if (!ReadFile(file, body + header_n, file_size, &bytes_read, nullptr)) {
        ExitProcess(1);
    }
    if (bytes_read != file_size) {
        ExitProcess(1);
    }

    // Print the body postfix after the data file (overflow already checked)
    memcpy(body + header_n + file_size, body_postfix, sizeof(body_postfix) - 1);


    srand(time(NULL));
    uint64_t backoff = 250; // milliseconds

    bool uploaded = false;

    // Upload crash dump
    for (int i = 0; i < 8; ++i) {

        // Windows HTTPS initialization
        HINTERNET hSession = WinHttpOpen(L"Discord Crashdump Webhook",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) continue;
        defer { WinHttpCloseHandle(hSession); };

        // Connect to domain
        HINTERNET hConnect = WinHttpConnect(hSession, L"discord.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) continue;
        defer { WinHttpCloseHandle(hConnect); };

        // Begin POST request to the discord webhook endpoint
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST",
            L"" DISCORD_WEBHOOK_ENDPOINT,
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest) continue;
        defer { WinHttpCloseHandle(hRequest); };

        // Send request once - don't handle auth challenge, credentials, reauth, redirects
        const wchar_t ContentType[] = L"Content-Type: multipart/form-data; boundary=19024605111143684786787635207";
        if (!WinHttpSendRequest(hRequest, ContentType, sizeof(ContentType) / sizeof(ContentType[0]),
            body, body_n, body_n, 0)) continue;

        // Wait for response
        if (!WinHttpReceiveResponse(hRequest, nullptr)) continue;

        // Pull headers from response
        DWORD dwStatusCode, dwSize = sizeof(dwStatusCode);
        if (!WinHttpQueryHeaders(hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            nullptr, &dwStatusCode, &dwSize, nullptr)) continue;
        if (dwStatusCode != 200) continue;

        uploaded = true;
        if (uploaded) {
            break;
        }

        // Retry internet connection with exponential backoff
        backoff *= 1.0f + ((float)rand() / RAND_MAX);
        printf("Crash dump upload failed, retrying in %0.3fs...\n", backoff * 0.001f);
        Sleep((DWORD)backoff);
    }

    // Cleanup
    CloseHandle(file);

    // If the file was successfully sent, we don't need it on disk anymore
    if (uploaded) {
        DeleteFileW((LPCWSTR)filename16);
    }

    // Return 1 because the app crashed, not because the crash report failed
    ExitProcess(1);
}
