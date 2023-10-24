# ArrangeInstances
Arranges windows that match a given regular expression to maximize screen space usage

## Building
1. Open Developer Command Prompt for VS 2022
1. `cd` to this repo.
1. Run `cl /EHsc /std:c++17 ArrangeInstances.cpp /link User32.lib`. This will generate `ArrangeInstances.exe`.

## Running
This tool expects two arguments:
1. A target aspect ratio desired for all instances, represented as a single floating point number, the result of `width/height`.
1. A regular expression parsed by the `std::regex` engine. The expression should be placed in quotation marks to match against window titles that should be arranged.
```
Usage: ArrangeInstances.exe <instanceRatio (width/height)> <ProcessRegEx>
        Example: ArrangeInstances.exe 1.33333333333 "Dolphin.* \|.*"
        Example: ArrangeInstances.exe 0.666666667 ".*melonDS .*"
        Example: ArrangeInstances.exe 1.77777777777 "yuzu Mainline.*"
```
