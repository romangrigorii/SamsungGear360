#include "logging.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::filesystem::path executableParentDirectory()
{
    namespace fs = std::filesystem;
#ifdef _WIN32
    char buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return fs::current_path();
    try {
        fs::path p(std::string(buf, buf + n));
        p = p.parent_path();
        if (p.empty())
            return fs::current_path();
        return fs::weakly_canonical(p);
    } catch (...) {
        return fs::current_path();
    }
#else
    return fs::current_path();
#endif
}

// Fallback when argv0 is a reliable path (non-Windows or GetModuleFileName unused).
std::filesystem::path directoryFromArgv0(const char* argv0)
{
    namespace fs = std::filesystem;
    try {
        fs::path p(argv0 ? argv0 : "");
        if (p.empty())
            return fs::current_path();
        if (p.is_relative())
            p = fs::weakly_canonical(fs::absolute(p));
        else
            p = fs::weakly_canonical(p);
        return p.parent_path();
    } catch (...) {
        return fs::current_path();
    }
}

std::string timestampUtcIso()
{
    using clock = std::chrono::system_clock;
    auto tp = clock::now();
    std::time_t t = clock::to_time_t(tp);
#ifdef _WIN32
    std::tm tmBuf{};
    gmtime_s(&tmBuf, &t);
    const std::tm* ptm = &tmBuf;
#else
    std::tm tmBuf{};
    gmtime_r(&t, &tmBuf);
    const std::tm* ptm = &tmBuf;
#endif
    std::ostringstream oss;
    oss << std::put_time(ptm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace

// Do not use freopen / _wfreopen_s: redirecting stdout+stderr to the same file has caused
// UCRT crashes. Console output stays on the console; this only appends a session line to disk.
bool initLoggingToFile(int argc, char** argv, const std::string& logBasename)
{
    (void)argc;
    std::filesystem::path dir;
    try {
#ifdef _WIN32
        dir = executableParentDirectory();
#else
        const char* argv0 = (argv && argv[0]) ? argv[0] : "";
        dir = directoryFromArgv0(argv0);
#endif
    } catch (...) {
        dir = std::filesystem::current_path();
    }
    const std::filesystem::path logPath = dir / logBasename;

    try {
        std::ofstream out(logPath, std::ios::app | std::ios::binary);
        if (!out)
            return false;
        out << "----- " << timestampUtcIso() << " session start -----\n";
        out.flush();
        return static_cast<bool>(out);
    } catch (...) {
        return false;
    }
}
