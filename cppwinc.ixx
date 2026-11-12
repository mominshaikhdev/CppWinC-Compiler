// cppwinc.ixx — module cppwinc.
// CppWinC: Unified WinRT / Win32 / Driver Projection Compiler for C++23.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.
//
// The original 4649-line monolith is split into consistent naming
// .ixx files for readability.  Each part is #included below within
// the single module's namespace scope.

module;

#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <mutex>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <ios>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <ranges>
#include <exception>
#include <memory>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <iterator>
#include <sstream>
#include <regex>
#include <unordered_map>
#include <unordered_set>
#include <atomic>

export module cppwinc;

import winmd_reader;
import strings_data;
import component_data;
import winuic_data;
import win32_data;
import wdk_data;

namespace cppwinc
{
#include "cppwinc_core.ixx"
#include "cppwinc_cli.ixx"
#include "cppwinc_types.ixx"
#include "cppwinc_proj.ixx"
#include "cppwinc_comp.ixx"
#include "cppwinc_driver.ixx"
}
