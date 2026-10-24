#include <windows.h>

#include <string>
#include <vector>

namespace {

std::wstring quote_argument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;

    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }

    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t bash[MAX_PATH];
    const DWORD count = GetEnvironmentVariableW(L"PS5_GIT_BASH", bash, MAX_PATH);
    if (count == 0 || count >= MAX_PATH) {
        fwprintf(stderr, L"PS5_GIT_BASH must point to Git Bash\\bin\\bash.exe\n");
        return 2;
    }

    std::wstring command = quote_argument(bash) + L" -c " + quote_argument(
        L"exec \"$(cygpath -u \"$PS5_PAYLOAD_SDK\")/bin/prospero-lld.sh\" \"$@\"")
        + L" prospero-lld";
    for (int index = 1; index < argc; ++index) {
        command += L" " + quote_argument(argv[index]);
    }

    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION process{};
    if (!CreateProcessW(bash, mutable_command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        fwprintf(stderr, L"Could not start Git Bash (error %lu)\n", GetLastError());
        return 2;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}
