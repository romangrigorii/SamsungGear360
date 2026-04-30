#pragma once

#include <string>

// Best-effort: appends one line to <exe_dir>/gear360_viewer.log. Does not redirect
// stdout/stderr (avoids UCRT freopen crashes and keeps the console usable).
bool initLoggingToFile(int argc, char** argv, const std::string& logBasename = "gear360_viewer.log");
