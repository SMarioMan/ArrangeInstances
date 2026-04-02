#include <Windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <regex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "User32.lib")

// Maps each HWND to its original styles.
struct StyleData {
  LONG_PTR style;
  LONG_PTR exStyle;
};

std::unordered_map<HWND, StyleData> savedStyles;
HANDLE exitEvent = NULL;

void CaptureStyle(HWND hWnd) {
  savedStyles[hWnd] = {GetWindowLongPtr(hWnd, GWL_STYLE),
                       GetWindowLongPtr(hWnd, GWL_EXSTYLE)};
}

void RestoreStyle(HWND hWnd) {
  auto it = savedStyles.find(hWnd);

  // Never captured, nothing to do.
  if (it == savedStyles.end()) {
    return;
  }

  // Window was closed while we were running.
  if (!IsWindow(hWnd)) {
    savedStyles.erase(it);
    return;
  }

  SetWindowLongPtr(hWnd, GWL_STYLE, it->second.style);
  SetWindowLongPtr(hWnd, GWL_EXSTYLE, it->second.exStyle);
  SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

  savedStyles.erase(it);
}

void RestoreAllStyles() {
  std::vector<HWND> keys;
  keys.reserve(savedStyles.size());
  for (const auto& [hwnd, _] : savedStyles) {
    keys.push_back(hwnd);
  }
  for (HWND hwnd : keys) {
    RestoreStyle(hwnd);
  }
}

void DisableStyle(HWND hWnd) {
  // The styles to disable.
  const LONG_PTR mask = WS_CAPTION | WS_BORDER | WS_DLGFRAME | WS_SYSMENU |
                        WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
  LONG_PTR currentStyle = GetWindowLongPtr(hWnd, GWL_STYLE);
  SetWindowLongPtr(hWnd, GWL_STYLE, currentStyle & ~mask);
}

void SetTaskbarAutoHide(bool enable) {
  HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
  if (!hTaskbar) return;
  APPBARDATA abd = {};
  abd.cbSize = sizeof(APPBARDATA);
  abd.hWnd = hTaskbar;
  abd.lParam = enable ? ABS_AUTOHIDE : 0;
  SHAppBarMessage(ABM_SETSTATE, &abd);
  ShowWindow(hTaskbar, enable ? SW_HIDE : SW_SHOWNOACTIVATE);
}

std::unordered_set<DWORD> GetExcludedProcessIds() {
  std::unordered_set<DWORD> excluded;

  // Ignore ancestors of the process calling this program.
  HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (hSnapshot != INVALID_HANDLE_VALUE) {
    std::unordered_map<DWORD, DWORD> parentMap;
    PROCESSENTRY32 pe = {};
    pe.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(hSnapshot, &pe)) {
      do {
        parentMap[pe.th32ProcessID] = pe.th32ParentProcessID;
      } while (Process32Next(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);

    DWORD current = GetCurrentProcessId();
    while (true) {
      auto it = parentMap.find(current);
      if (it == parentMap.end() || it->second == 0 || it->second == current)
        break;
      excluded.insert(it->second);
      current = it->second;
    }
  }

  // Exclude conhost.
  HWND hConsole = GetConsoleWindow();
  if (hConsole) {
    DWORD conhostPid = 0;
    GetWindowThreadProcessId(hConsole, &conhostPid);
    excluded.insert(conhostPid);
  }

  // Exclude the visible terminal host (e.g. Windows Terminal via ConPTY).
  // ConPTY has no HWND ancestry relationship with conhost, so we locate the
  // terminal window by matching its title against the current console title.
  char consoleTitle[MAX_PATH] = {};
  if (GetConsoleTitleA(consoleTitle, MAX_PATH) > 0) {
    HWND hTerminal = FindWindowA(NULL, consoleTitle);
    if (hTerminal) {
      DWORD terminalPid = 0;
      GetWindowThreadProcessId(hTerminal, &terminalPid);
      excluded.insert(terminalPid);  // Windows Terminal PID
    }
  }

  return excluded;
}

std::string FormatFileTime(const FILETIME& ft) {
  SYSTEMTIME st{};
  FILETIME local{};
  FileTimeToLocalFileTime(&ft, &local);
  FileTimeToSystemTime(&local, &st);

  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear,
           st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  return buf;
}

FILETIME GetWindowCreationTime(HWND hWnd) {
  DWORD processId;
  GetWindowThreadProcessId(hWnd, &processId);

  HANDLE hProcess =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
  if (hProcess == NULL) return FILETIME{};

  FILETIME creationTime, exitTime, kernelTime, userTime;
  if (!GetProcessTimes(hProcess, &creationTime, &exitTime, &kernelTime,
                       &userTime)) {
    CloseHandle(hProcess);
    return FILETIME{};
  }

  CloseHandle(hProcess);
  return creationTime;
}

struct EnumWindowsParams {
  EnumWindowsParams(const std::regex& regex)
      : regex(regex), excludedPids(GetExcludedProcessIds()) {}
  std::vector<HWND> matchingWindows;
  const std::regex regex;
  const std::unordered_set<DWORD> excludedPids;
};

std::string WindowTitle(HWND hWnd) {
  int length = GetWindowTextLength(hWnd);
  if (length == 0) return "";
  std::vector<char> buffer(length + 1);
  GetWindowTextA(hWnd, buffer.data(), length + 1);
  return std::string(buffer.data());
}

BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam) {
  EnumWindowsParams* params = reinterpret_cast<EnumWindowsParams*>(lParam);
  if (!params) return FALSE;

  // Skip excluded windows.
  DWORD windowPid;
  GetWindowThreadProcessId(hWnd, &windowPid);
  if (params->excludedPids.count(windowPid)) return TRUE;

  // Check if the window title matches the regular expression.
  std::string windowTitle = WindowTitle(hWnd);
  if (std::regex_match(windowTitle, params->regex)) {
    params->matchingWindows.push_back(hWnd);
  }

  return TRUE;
}

bool WindowOrderCmp(HWND hWnd1, HWND hWnd2) {
  FILETIME ft1 = GetWindowCreationTime(hWnd1);
  FILETIME ft2 = GetWindowCreationTime(hWnd2);
  int cmp = CompareFileTime(&ft1, &ft2);
  if (cmp != 0) return cmp < 0;
  return WindowTitle(hWnd1) < WindowTitle(hWnd2);
}

void SortWindows(std::vector<HWND>& hWndVector) {
  std::sort(hWndVector.begin(), hWndVector.end(), WindowOrderCmp);
}

std::vector<HWND> GetProcesses(const std::regex& regex) {
  EnumWindowsParams params(regex);
  EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&params));

  SortWindows(params.matchingWindows);

  std::cout << "Found " << params.matchingWindows.size()
            << " matching windows:" << std::endl;
  for (const auto window : params.matchingWindows) {
    std::cout << window << "\t" << FormatFileTime(GetWindowCreationTime(window))
              << "\t" << WindowTitle(window) << std::endl;
  }

  return params.matchingWindows;
}

// Explanation of how the optimal tiling is found in constant time:
// Constraint: To display all instances, we want to have N instances tiled in a
// rows * columns grid, so:
// • rows * columns = N
// Constraint: To maximize screen usage, we want the number of columns and rows
// to reshape the effective instanceRatio to match the screen's ratio, so:
// • (columns / rows) * instanceRatio = screenRatio
// N, instanceRatio, and screenRatio are known.
// To find the optimal tiling, solve the system of equations.
// • rows * columns = N
// • columns = N/rows
// Substitution:
// • (columns / rows) * instanceRatio = screenRatio
// • (N / rows^2) * instanceRatio = screenRatio
// • rows^2 = instanceRatio/(screenRatio*N)
// • rows = sqrt(instanceRatio/(screenRatio*N))
// Then, since we know the number of rows:
// • columns = N/rows
// Which solves for columns.
// If we could have fractional rows and columns, as the system of equations
// solves for, we would perfectly fill the screen every time. Since this is not
// possible, we use the fractional part of the rows and columns count to
// determine which needs an extra row/column to accommodate all instances.
// Whichever remainder is higher should work better.
// Sometimes it's still not enough and we must add both an extra row and an
// extra column.
std::tuple<std::size_t, std::size_t> GetOptimalTiling(
    double screenRatio, double instanceRatio, std::size_t instanceCount) {
  double targetRatio = screenRatio / instanceRatio;
  double h = sqrt(instanceCount / targetRatio);
  double w = instanceCount / h;
  std::cout << "h: " << h << "\t"
            << "w: " << w << std::endl;
  std::size_t numTall = static_cast<std::size_t>(h);
  std::size_t numWide = static_cast<std::size_t>(w);
  // Determine if rounding must occur.
  double remTall = h - numTall;
  double remWide = w - numWide;
  std::cout << "numTall: " << numTall << "\t"
            << "numWide: " << numWide << std::endl;
  std::cout << "remTall: " << remTall << "\t"
            << "remWide: " << remWide << std::endl;
  // If we do not have enough space to fit all instances, add an extra row or
  // column, whichever minimizes the violation of the target aspect ratio.
  // Decrement the remainder to ensure we alternate between increasing the
  // number of rows or columns until we can fit the desired instance count.
  while (numTall * numWide < instanceCount) {
    if (remTall > remWide) {
      numTall++;
      remTall--;
    } else {
      numWide++;
      remWide--;
    }
  }
  std::cout << "numTall: " << numTall << "\t"
            << "numWide: " << numWide << std::endl;
  return {numWide, numTall};
}

std::tuple<int, int> GetDesktopResolution() {
  RECT desktop;
  const HWND hDesktop = GetDesktopWindow();
  // Get the size of screen to the variable desktop
  GetWindowRect(hDesktop, &desktop);
  // The top left corner will have coordinates (0,0)
  // and the bottom right corner will have coordinates
  // (horizontal, vertical)
  return {static_cast<std::size_t>(desktop.right),
          static_cast<std::size_t>(desktop.bottom)};
}

void PlaceWindows(const std::vector<HWND>& instances, double instanceRatio,
                  const std::optional<std::tuple<std::size_t, std::size_t>>&
                      tiling = std::nullopt) {
  const auto [screenW, screenH] = GetDesktopResolution();
  std::size_t numWide, numTall;
  if (!tiling) {
    double screenRatio =
        static_cast<double>(screenW) / static_cast<double>(screenH);
    std::tie(numWide, numTall) =
        GetOptimalTiling(screenRatio, instanceRatio, instances.size());
  } else {
    std::tie(numWide, numTall) = tiling.value();
  }
  // Dimensions of each instance.
  const int width = screenW / numWide;
  const int height = screenH / numTall;
  // Reposition each instance.
  for (std::size_t i = 0; i < instances.size(); i++) {
    const int xPos = (i % numWide) * width;
    const int yPos = (i / numWide) * height;
    SetWindowPos(instances[i], NULL, xPos, yPos, width, height, SWP_SHOWWINDOW);
  }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
      signal == CTRL_CLOSE_EVENT) {
    // Signal the main to wake up and clean up, then stall this thread so the
    // process isn't killed before main finishes.
    SetEvent(exitEvent);
    Sleep(3000);
    return TRUE;
  }
  return FALSE;
}

int main(int argc, char* argv[]) {
  if (argc <= 2) {
    std::cout << "Usage: " << argv[0]
              << " <instanceRatio (width/height)> <ProcessRegEx>\n"
              << "\tExample: " << argv[0]
              << " 1.33333333333 \"Dolphin.* \\|.*\"" << std::endl
              << "\tExample: " << argv[0] << " 0.666666667 \".*melonDS .*\""
              << std::endl
              << "\tExample: " << argv[0]
              << " 1.77777777777 \"yuzu Mainline.*\"" << std::endl;
    return -1;
  }
  const double instanceRatio = std::stod(argv[1]);
  const std::string procRegexStr = argv[2];

  std::regex procRegex(procRegexStr, std::regex_constants::grep);

  std::vector<HWND> instances = GetProcesses(procRegex);
  if (instances.empty()) {
    std::cout << "No matching windows found." << std::endl;
    return 0;
  }

  exitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
  SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

  for (const HWND hwnd : instances) {
    CaptureStyle(hwnd);
    DisableStyle(hwnd);
  }
  SetTaskbarAutoHide(true);
  PlaceWindows(instances, instanceRatio);

  std::cout << "Running. Close this window to restore and exit." << std::endl;
  WaitForSingleObject(exitEvent, INFINITE);

  std::cout << "\nCleaning up..." << std::endl;
  SetTaskbarAutoHide(false);
  RestoreAllStyles();

  CloseHandle(exitEvent);
  return 0;
}
