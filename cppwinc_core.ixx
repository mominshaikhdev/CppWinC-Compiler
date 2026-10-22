// cppwinc_core.ixx — writer class, settings, foundation check
// Split from cppwinc.ixx for readability.
// This file is #included from cppwinc.ixx, not compiled independently.

// NuGet global packages folder: honor the NUGET_PACKAGES override first
// (users and CI routinely redirect it), then %USERPROFILE%\.nuget\packages.
// Shared by every WASDK discovery path (-winui metadata + runtime DLLs,
// bootstrap .lib/.dll locators). Never hardcodes a drive or user name.
inline std::filesystem::path nuget_packages_root()
{
    if (const wchar_t* env = _wgetenv(L"NUGET_PACKAGES"))
    {
        if (*env)
        {
            std::filesystem::path p(env);
            std::error_code ec;
            if (std::filesystem::is_directory(p, ec)) return p;
        }
    }
    if (const wchar_t* home = _wgetenv(L"USERPROFILE"))
    {
        return std::filesystem::path(home) / L".nuget" / L"packages";
    }
    return {};
}

// Check if a type is in Windows.Foundation or Microsoft.Windows.Foundation.
// WASDK 2.3.x ships Microsoft.Windows.Foundation.winmd which uses
// Microsoft.Windows.Foundation alongside Windows.Foundation.
inline bool is_foundation_type(std::string_view ns, std::string_view name = "")
{
    if (ns == "Windows.Foundation" || ns == "Microsoft.Windows.Foundation")
    {
        if (name.empty()) return true;
        // Common Foundation types that appear in both namespaces
        return name == "IInspectable" || name == "IAsyncAction"
            || name == "IAsyncOperation" || name == "IAsyncActionWithProgress"
            || name == "IAsyncOperationWithProgress" || name == "IAsyncInfo"
            || name == "EventRegistrationToken" || name == "HResult"
            || name == "DateTime" || name == "TimeSpan"
            || name == "Rect" || name == "Point" || name == "Size"
            || name == "PropertyValue" || name == "Guid";
    }
    return false;
}

// writer

class writer
{
    std::string m_buffer;

    void append(std::string_view value) { m_buffer += value; }
    void append(std::string const& value) { m_buffer += value; }
    void append(char const* value) { m_buffer += value; }
    void append(char value) { m_buffer += value; }
    void append(wchar_t const*) = delete;

    template <typename T>
    void append(T value) { m_buffer += std::to_string(value); }

    template <typename... Args>
    void substitute(std::string_view format, Args&&... args)
    {
        auto pos = std::size_t{ 0 };
        auto apply = [&](auto&& value)
        {
            auto marker = format.find('%', pos);
            if (marker == std::string_view::npos)
            {
                append(format.substr(pos));
                pos = format.size();
                return;
            }
            if (marker + 1 < format.size() && format[marker + 1] == '%')
            {
                append(format.substr(pos, marker - pos + 1));
                pos = marker + 2;
                return;
            }
            append(format.substr(pos, marker - pos));
            pos = marker + 1;
            append(std::forward<decltype(value)>(value));
        };
        (apply(std::forward<Args>(args)), ...);
        if (pos < format.size())
        {
            append(format.substr(pos));
        }
    }

public:
    template <typename... Args>
    void write(std::string_view format, Args&&... args)
    {
        substitute(format, std::forward<Args>(args)...);
    }

    std::string const& str() const noexcept { return m_buffer; }

    void flush_to_file(std::filesystem::path const& path) const
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("cannot write file: " + path.string());
        }
        file.write(m_buffer.data(), static_cast<std::streamsize>(m_buffer.size()));
    }
};

// settings

struct settings_type
{
    std::vector<std::filesystem::path> input;
    std::set<std::string, std::less<>> input_dbs;
    std::string output_folder = ".";
    bool verbose = false;
    bool base = false;
    bool component = false;
    std::string component_folder;
    std::string component_name;
    // Canonical paths of the app .winmd(s) — every non-system winmd named
    // in -input, regardless of position. Component classes only come from
    // app winmds — every type in a system/projection input db must never
    // qualify, or the component module would absorb the whole OS surface.
    std::set<std::string, std::less<>> app_dbs;
    std::set<std::string, std::less<>> include;
    std::set<std::string, std::less<>> exclude;
    std::set<std::string, std::less<>> module_include;
    std::set<std::string, std::less<>> module_exclude;
    std::string winui_dll_path;
    bool winui = false;
    bool winui_no_compile = false;
    // -compile: run the built-in module compilation pass (same machinery as
    // -winui) without WASDK auto-discovery — for winrt/win32/wdk projections.
    bool compile = false;
    // -target exe,sys,dll — explicit link targets (comma-separated,
    // case-insensitive). Empty = auto-detect from project sources.
    std::set<std::string, std::less<>> targets;
    // Folder from the -compile/-winui path switches that receives ONLY the
    // final linked binaries (exe/sys/dll); generated modules and build
    // intermediates stay with -output. Empty = -output folder.
    std::string final_output;
};

settings_type settings;

