// Build command:
// cl /EHsc /std:c++17 ArrangeInstances.cpp /link User32.lib

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <regex>
#include <tuple>
#include <vector>

bool skip = true;

// TODO: This is non-functional.
BOOL CALLBACK EnumChildProc(HWND hWnd, LPARAM lParam) {
  char class_name[80];
  GetClassName(hWnd, class_name, sizeof(class_name));
  std::cout << "DEBUG: " << class_name << std::endl;
  if (strcmp(class_name, "Qt5153QWindowIcon") == 0) {
    if (skip) {
      skip = false;
      std::cout << "Skipping first Qt5153QWindowIcon" << std::endl;
    } else {
      // RemoveWindowSubclass(hwnd, (SUBCLASSPROC)SubclassProc, uIdSubclass);
      EnableWindow(hWnd, FALSE);
      std::cout << "Disabled second Qt5153QWindowIcon" << std::endl;
      return FALSE;
    }
  }
  return TRUE;
}

void DisableWindowStyle(const HWND& hWnd) {
  // The styles to disable.
  const LONG_PTR style = WS_DLGFRAME |               // Dialog frame
                         WS_SIZEBOX |                // Sizing border
                         WS_BORDER |                 // Thin-line border
                         WS_CAPTION |                // Title bar
                         WS_TILEDWINDOW | 0xC40000;  // "Full state"
  LONG_PTR currentStyle = GetWindowLongPtr(hWnd, GWL_STYLE);
  SetWindowLongPtr(hWnd, GWL_STYLE, currentStyle & ~style);

  skip = true;
  if (!EnumChildWindows(hWnd, EnumChildProc, NULL)) {
    std::cout << "Failed to remove Qt5153QWindowIcon2" << std::endl;
  }
}

FILETIME GetWindowCreationTime(HWND hWnd) {
  DWORD processId;
  GetWindowThreadProcessId(hWnd, &processId);

  HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                FALSE, processId);
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

bool CompareHwndByCreationTime(HWND hWnd1, HWND hWnd2) {
  FILETIME ft1 = GetWindowCreationTime(hWnd1);
  FILETIME ft2 = GetWindowCreationTime(hWnd2);
  return CompareFileTime(&ft1, &ft2) <
         0;  // Returns true if ft1 is earlier than ft2
}

void SortByCreation(std::vector<HWND>& hWndVector) {
  std::sort(hWndVector.begin(), hWndVector.end(), CompareHwndByCreationTime);
}

struct MyParams {
  MyParams(const std::regex& regex) : regex(regex) {}
  std::vector<HWND> matchingWindows;
  const std::regex regex;
};

std::string WindowTitle(const HWND& hWnd) {
  int length = GetWindowTextLength(hWnd);
  char* buffer = new char[length + 1];
  if (!buffer) return "Error";
  GetWindowText(hWnd, buffer, length + 1);
  std::string windowTitle(buffer);
  delete[] buffer;
  return windowTitle;
}

BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam) {
  MyParams* params = reinterpret_cast<MyParams*>(lParam);
  if (!params) return FALSE;

  std::string windowTitle = WindowTitle(hWnd);

  // Check if the window title matches the regular expression.
  if (std::regex_match(windowTitle, params->regex)) {
    std::cout << "Window with title \"" << windowTitle << "\" matched!"
              << std::endl;
    params->matchingWindows.push_back(hWnd);
  }

  return TRUE;
}

std::vector<HWND> GetProcesses(const std::regex& regex) {
  MyParams params(regex);
  EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&params));

  std::cout << "Found " << params.matchingWindows.size()
            << " matching windows:" << std::endl;
  for (const auto window : params.matchingWindows) {
    std::cout << window << "\t" << WindowTitle(window) << std::endl;
  }

  SortByCreation(params.matchingWindows);

  return params.matchingWindows;
}

void FocusProcesses(const std::vector<HWND>& processes) {
  for (const auto process : processes) {
    SetForegroundWindow(process);
  }
  HWND hDesktop = GetDesktopWindow();
  SetFocus(hDesktop);
  return;
}

std::tuple<int, int> GetOptimalTiling(const double& screenRatio,
                                      const double& instanceRatio,
                                      const int& instanceCount) {
  double targetRatio = screenRatio / instanceRatio;
  double h = sqrt(instanceCount / targetRatio);
  double w = instanceCount / h;
  std::cout << "h: " << h << "\t"
            << "w: " << w << std::endl;
  int numTall = static_cast<int>(h);
  int numWide = static_cast<int>(w);
  // Detemine if rounding must occur.
  double remTall = h - numTall;
  double remWide = w - numWide;
  std::cout << "numTall: " << numTall << "\t"
            << "numWide: " << numWide << std::endl;
  std::cout << "remTall: " << remTall << "\t"
            << "remWide: " << remWide << std::endl;
  if (remTall > 0 || remWide > 0) {
    // TODO: Could floating point precision errors make us think we need to do
    // this, even when we don't?
    if (remTall > remWide) {
      numTall++;
      if (numTall * numWide < instanceCount) {
        numWide++;
      }
    } else {
      numWide++;
      if (numTall * numWide < instanceCount) {
        numTall++;
      }
    }
  }
  std::cout << "numTall: " << numTall << "\t"
            << "numWide: " << numWide << std::endl;
  return std::make_tuple(numWide, numTall);
}

std::tuple<int, int> GetDesktopResolution() {
  RECT desktop;
  const HWND hDesktop = GetDesktopWindow();
  // Get the size of screen to the variable desktop
  GetWindowRect(hDesktop, &desktop);
  // The top left corner will have coordinates (0,0)
  // and the bottom right corner will have coordinates
  // (horizontal, vertical)
  return std::make_tuple(desktop.right, desktop.bottom);
}

void PlaceWindows(
    const std::vector<HWND>& instances, const double& instanceRatio,
    const std::optional<std::tuple<int, int>>& tiling = std::nullopt) {
  const auto [screenW, screenH] = GetDesktopResolution();
  int numWide, numTall;
  if (tiling == std::nullopt) {
    double screenRatio = (double)screenW / (double)screenH;
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
    BOOL res = SetWindowPos(instances[i], NULL, xPos, yPos, width, height,
                            SWP_SHOWWINDOW);
  }
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

  // Get matching proceses.
  std::vector<HWND> instances = GetProcesses(procRegex);

  for (const auto instance : instances) {
    DisableWindowStyle(instance);
  }

  PlaceWindows(instances, instanceRatio);

  return 0;
}
