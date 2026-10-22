// cppwinc_types.ixx — type mapping, naming, signatures, SCC
// Split from cppwinc.ixx for readability.
// This file is #included from cppwinc.ixx, not compiled independently.

// naming

std::string ns_cpp(std::string_view ns)
{
    std::string result = "winrt::";
    std::string_view rest = ns;
    bool first = true;
    while (!rest.empty())
    {
        auto dot = rest.find('.');
        std::string_view part = dot == std::string_view::npos ? rest : rest.substr(0, dot);
        if (!first)
        {
            result += "::";
        }
        result += part;
        rest = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
        first = false;
    }
    return result;
}

// Header-version namespaces carry dot segments that are not valid C++
// identifiers (Windows.Wdk.BITS1.5, Windows.Wdk.D2D1.1, ...). Mangle each
// dot-separated segment into one valid identifier: a segment that starts
// with a digit merges into the previous segment with an underscore
// (BITS1.5 -> BITS1_5), interior invalid characters become underscores.
// Returned segments are joined by the caller with the separator it needs.
std::vector<std::string> mangle_ns_segments(std::string const& ns)
{
    std::vector<std::string> raw;
    std::string cur;
    for (char c : ns)
    {
        if (c == '.')
        {
            raw.push_back(std::move(cur));
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    raw.push_back(std::move(cur));
    std::vector<std::string> clean;
    for (auto& p : raw)
    {
        if (p.empty())
        {
            clean.emplace_back("_");
            continue;
        }
        std::string seg;
        bool merged_leading_digit = false;
        for (std::size_t i = 0; i < p.size(); ++i)
        {
            char c = p[i];
            bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            bool ident = alnum || c == '_';
            if (!ident)
            {
                c = '_';
            }
            if (seg.empty() && (c >= '0' && c <= '9'))
            {
                // digit-led segment ("5" in BITS1.5): merge into the
                // previous segment rather than open an unnamed number scope
                if (!clean.empty())
                {
                    clean.back() += '_';
                    clean.back() += c;
                    merged_leading_digit = true;
                    continue;
                }
                seg += '_';
            }
            seg += c;
        }
        if (seg.empty() && !merged_leading_digit)
        {
            seg = "_";
        }
        if (!seg.empty())
        {
            clean.push_back(std::move(seg));
        }
    }
    return clean;
}

// Win32 opaque handle aliases: winversec emits DECLARE_HANDLE results
// (HINSTANCE, HANDLE, HWND, ...) as field-less/method-less classes. These
// project as void* (ABI-exact); anything else keeps its declared shape so
// genuine struct-like classes are never erased.
inline bool win32_handle_alias(std::string_view name)
{
    if (name.empty()) return false;
    if (name.starts_with("H")) return true;  // HINSTANCE, HWND, HMENU, ...
    static const std::set<std::string_view, std::less<>> aliases = {
        "HANDLE", "SC_HANDLE", "SOCKET", "HINTERNET", "HCRYPTPROV",
        "HCRYPTKEY", "HCRYPTHASH", "HCERTSTORE", "HCRYPTMSG", "HDEVNOTIFY",
        "HPOWERNOTIFY", "HDROP", "HWINSTA", "HDESK", "HGDIOBJ", "HGLOBAL",
        "HLOCAL", "HRSRC", "PROCESSCALLBACK", "LSTATUS", "SECURITY_STATUS",
    };
    return aliases.count(name) != 0;
}

// Win32/WDK namespace: plain C++ namespace
std::string win32_ns_cpp(std::string_view ns)
{
    std::string result;
    for (auto const& s : mangle_ns_segments(std::string(ns)))
    {
        if (!result.empty())
        {
            result += "::";
        }
        result += s;
    }
    return result;
}

// Win32/WDK type name: type lives in its actual namespace
std::string win32_type_cpp_name(winmd::type_info const& type)
{
    return win32_ns_cpp(type.ns) + "::" + type.name;
}

std::string type_cpp_name(winmd::type_info const& type)
{
    return ns_cpp(type.ns) + "::" + type.name;
}

std::string primitive_cpp(std::uint8_t kind)
{
    // MSVC's std module does not surface the unqualified <cstdint>
    // typedefs (C3064/C3646), so qualify them.
    switch (kind)
    {
    case winmd::et_void: return "void";
    case winmd::et_boolean: return "bool";
    case winmd::et_char: return "char16_t";
    case winmd::et_i1: return "std::int8_t";
    case winmd::et_u1: return "std::uint8_t";
    case winmd::et_i2: return "std::int16_t";
    case winmd::et_u2: return "std::uint16_t";
    case winmd::et_i4: return "std::int32_t";
    case winmd::et_u4: return "std::uint32_t";
    case winmd::et_i8: return "std::int64_t";
    case winmd::et_u8: return "std::uint64_t";
    case winmd::et_r4: return "float";
    case winmd::et_r8: return "double";
    case winmd::et_string: return "winrt::hstring";
    case winmd::et_object: return "winrt::IInspectable";
    default: return {};
    }
}

// Enum underlying types must resolve to a fundamental type.
std::string enum_underlying_cpp(std::uint8_t kind)
{
    return primitive_cpp(kind);
}

// type mapper (metadata type -> C++ name)
//
// The mapper is context-sensitive: set m_proj_model to control how
// WinRT-specific and Win32-specific types are mapped.
//   0 = WinRT  (et_object -> IInspectable, et_szarray -> com_array, fallback -> IInspectable)
//   1 = Win32/WDK (et_object -> void*, et_szarray -> bare, fallback -> void*)

class type_mapper
{
    winmd::metadata const& m_metadata;
    mutable int m_proj_model = 0; // 0=winrt, 1=win32_wdk
    mutable std::string m_owner_ns; // current module owner namespace (e.g. "Windows")
    // True while mapping a pointer pointee: opaque/unresolvable types then
    // map to "void" so the enclosing Ptr levels fold into the exact stars
    // (IInspectable** = Ptr(Ptr(Class)) -> void**). In value context they
    // must map to "void*" — "void" there emits illegal `void field;` (C2182).
    mutable bool m_pointee = false;

public:
    explicit type_mapper(winmd::metadata const& metadata) : m_metadata(metadata) {}

    // Set the projection model for subsequent map() calls.
    void set_proj_model(int model) const { m_proj_model = model; }
    int proj_model() const { return m_proj_model; }

    // Set the current module owner namespace for short-name resolution.
    void set_owner_ns(std::string const& ns) const { m_owner_ns = ns; }
    std::string const& owner_ns() const { return m_owner_ns; }

    std::string map(winmd::type const& t) const
    {
        if (t.kind == winmd::et_var || t.kind == winmd::et_mvar)
        {
            // WinRT property type tokens: when proj_model is WinRT (0),
            // small var indices that correspond to ECMA-335 element type
            // byte values represent concrete types (e.g. VAR(0x05)=UInt8).
            if (m_proj_model == 0 && t.var <= 0x1C)
            {
                static const char* prop_types[] = {
                    nullptr,        // 0x00
                    nullptr,        // 0x01 (void)
                    "bool",         // 0x02 (Boolean)
                    "char16_t",     // 0x03 (Char)
                    "std::int8_t",  // 0x04 (I1)
                    "std::uint8_t", // 0x05 (U1)
                    "std::int16_t", // 0x06 (I2)
                    "std::uint16_t",// 0x07 (U2)
                    "std::int32_t", // 0x08 (I4)
                    "std::uint32_t",// 0x09 (U4)
                    "std::int64_t", // 0x0A (I8)
                    "std::uint64_t",// 0x0B (U8)
                    "float",        // 0x0C (R4)
                    "double",       // 0x0D (R8)
                    "winrt::hstring",// 0x0E (String)
                    nullptr,        // 0x0F
                    nullptr,        // 0x10
                    nullptr,        // 0x11
                    nullptr,        // 0x12
                    nullptr,        // 0x13
                    nullptr,        // 0x14
                    nullptr,        // 0x15
                    nullptr,        // 0x16
                    nullptr,        // 0x17
                    nullptr,        // 0x18
                    nullptr,        // 0x19
                    nullptr,        // 0x1A
                    "winrt::IInspectable", // 0x1B (Object)
                    "winrt::IInspectable", // 0x1C (SZARRAY of Object)
                };
                if (auto pt = prop_types[t.var]) return pt;
            }
            // Generic vars in C-function context (Win32/WDK): a metadata
            // generic param has no C equivalent — project as void*.
            if (m_proj_model != 0)
                return "void*";
            return "T" + std::to_string(t.var);
        }
        if (t.kind == winmd::et_ptr)
        {
            // Winversec encodes every C pointer (LPCWSTR, PVOID, PSTR* ...)
            // as PTR(pointee). Project the pointee recursively; when it is a
            // fundamental element type (Char for wchar_t, U4 for DWORD*) the
            // mapper below yields the exact C type, and parameters add the
            // const from the CModReqd(Const) modifier. Unnamed/unknown
            // pointees stay void*.
            if (!t.children.empty())
            {
                bool prev = m_pointee;
                m_pointee = true;
                std::string pointee = map(t.children[0]);
                m_pointee = prev;
                if (t.children[0].pt_const && !pointee.empty() && pointee.back() != '*')
                    pointee += " const";
                return pointee + "*";
            }
            return "void*";
        }
        if (t.kind == winmd::et_szarray)
        {
            if (m_proj_model == 0)
            {
                // WinRT: arrays use com_array<T>
                return "winrt::com_array<" + (t.children.empty() ? std::string("std::uint8_t") : map(t.children[0])) + ">";
            }
            else
            {
                // Win32/WDK: bare pointer (array data + size pair handled by codegen)
                if (t.children.empty()) return "std::uint8_t*";
                return map(t.children[0]);
            }
        }
        std::string p = primitive_cpp(t.kind);
        if (!p.empty())
        {
            // C-function context (Win32/WDK): wchar_t is the native C type for
            // ET::Char (WCHAR, OLECHAR) — char16_t would break every
            // string-literal argument (L"..." is wchar_t*, not char16_t*).
            if (m_proj_model != 0 && t.kind == winmd::et_char)
                return "wchar_t";
            // C-function context (Win32/WDK): winrt-flavored primitives have
            // no C equivalent — project as opaque void pointers.
            if (m_proj_model != 0
                && (t.kind == winmd::et_string || t.kind == winmd::et_object))
                return "void*";
            return p;
        }
        if (t.kind == winmd::et_class || t.kind == winmd::et_valuetype || t.kind == winmd::et_genericinst)
        {
            auto* type = m_metadata.find(t);
            if (type)
            {
                if (t.kind == winmd::et_valuetype && is_foundation_type(type->ns, type->name) && type->name == "EventRegistrationToken")
                {
                    // Pointee folding: EventRegistrationToken* = void*,
                    // not void** (the token itself is not a pointer).
                    if (m_proj_model != 0)
                        return m_pointee ? "void" : "void*";
                    return "winrt::event_token";
                }
                if (is_foundation_type(type->ns, type->name) && type->name == "IInspectable")
                {
                    // Win32/WDK: "void" as a pointee — the sig's Ptr levels
                    // add the stars (IInspectable** = Ptr(Ptr(Class)) →
                    // void**). Returning "void*" here stacked an extra star
                    // onto every such signature (RoActivateInstance came out
                    // as void***). Outside a pointee (never happens in C —
                    // interfaces are always behind a pointer) keep void*.
                    if (m_proj_model != 0)
                        return m_pointee ? "void" : "void*";
                    return "winrt::IInspectable";
                }
                // Win32/WDK context: opaque types (winversec emits
                // DECLARE_HANDLE results as field-less/method-less classes:
                // HINSTANCE, HWND, PACKAGEDEPENDENCY_CONTEXT,
                // CO_MTA_USAGE_COOKIE, APARTMENT_SHUTDOWN_REGISTRATION_COOKIE,
                // ...) have no declared projection. Name-list matching missed
                // every handle name outside the hardcoded set and produced
                // C2061 'undeclared identifier' in every module whose
                // signatures mention them, so treat ANY member-less class in
                // Win32/WDK context as the opaque pointer it is — void* is
                // ABI-exact for every HANDLE-shaped type.
                if (m_proj_model != 0
                    && type->kind == winmd::type_kind::class_
                    && type->fields.empty() && type->methods.empty()
                    && type->interfaces.empty() && type->properties.empty())
                {
                    // Same pointee folding as the fallback below: as a
                    // pointee emit "void" (the enclosing Ptr supplies the
                    // star), in value context the opaque void* itself.
                    return m_pointee ? "void" : "void*";
                }
                // Always fully qualify. Out-of-line member definitions are
                // emitted at module scope (fully-qualified declarators), and in
                // C++ a return type is looked up in the enclosing scope BEFORE
                // the qualified declarator is parsed — unlike parameters, which
                // are looked up in the class scope. A short (unqualified) name
                // therefore cannot resolve at module scope and breaks every
                // getter that returns an own-namespace type (e.g. Foundation's
                // AsyncActionCompletedHandler). Cross-namespace references also
                // require full qualification because each namespace is emitted
                // as its own nested block.                // WDK/Win32 namespaces live under plain C++ namespaces
                // (win32_ns_cpp), not the winrt:: projection — a winrt::-typed
                // name in a WDK module breaks every Wdk compile (C2653).
                std::string result = (m_proj_model != 0)
                    ? win32_type_cpp_name(*type)
                    : type_cpp_name(*type);
                if (t.kind == winmd::et_genericinst)
                {
                    result += "<";
                    for (std::size_t i = 0; i < t.args.size(); ++i)
                    {
                        if (i) result += ", ";
                        result += map(t.args[i]);
                    }
                    result += ">";
                }
                return result;
            }
        }
        if (t.kind == winmd::et_object)
        {
            return (m_proj_model == 0) ? "winrt::IInspectable" : "void*";
        }
        // Unresolvable class/value-type in Win32/WDK mode: as a pointee,
        // return "void" — the sig's own Ptr levels (IInspectable** =
        // Ptr(Ptr(Class))) supply the stars. In value context (by-value
        // struct fields like CLS_IO_STATISTICS_HEADER::eStatsClass) "void"
        // is illegal (C2182), so keep the opaque void* there. WinRT mode
        // keeps the wrapper since WinRT signatures are not Ptr-wrapped.
        if (m_proj_model != 0)
        {
            return m_pointee ? "void" : "void*";
        }
        // Fallback: WinRT uses IInspectable
        return "winrt::IInspectable";
    }
};

// filtering

bool matches(std::set<std::string, std::less<>> const& prefixes, std::string const& full_name)
{
    for (auto const& prefix : prefixes)
    {
        if (full_name.starts_with(prefix))
        {
            return true;
        }
    }
    return false;
}

// Namespace classification: determines which projection model to use
enum class ns_projection_model
{
    winrt,   // Windows.* (excluding Win32/WDK sub-namespaces) -> winrt:: prefix
    win32,   // Windows.Win32.* -> plain C++ types, P/Invoke
    wdk,     // Windows.Wdk.* or Windows.Win32.Kernel.* -> kernel-mode types
};

bool is_win32_namespace(std::string_view ns)
{
    return ns.starts_with("Windows.Win32") && !ns.starts_with("Windows.Win32.Kernel");
}

bool is_wdk_namespace(std::string_view ns)
{
    return ns.starts_with("Windows.Wdk") || ns.starts_with("Windows.Win32.Kernel");
}

bool is_win32_type(winmd::type_info const& type)
{
    return is_win32_namespace(type.ns);
}

bool is_wdk_type(winmd::type_info const& type)
{
    return is_wdk_namespace(type.ns);
}

ns_projection_model classify_namespace(std::string_view ns)
{
    if (is_wdk_namespace(ns)) return ns_projection_model::wdk;
    if (is_win32_namespace(ns)) return ns_projection_model::win32;
    return ns_projection_model::winrt;
}

// Generate the C++ module name for a namespace (no prefix for Win32/WDK)
std::string module_name_for_namespace(std::string const& ns)
{
    auto model = classify_namespace(ns);
    if (model == ns_projection_model::win32 || model == ns_projection_model::wdk)
    {
        // Win32/WDK: module name matches namespace (e.g. "Windows.Win32.FileSystem")
        return ns;
    }
    // WinRT: module name prefixed with "winrt." (e.g. "winrt.Windows.Foundation")
    return "winrt." + ns;
}

// selective -winui type closure
// In -winui / -winui_no_compile mode the projection emits only the types
// reachable from the project's own code: component classes, view element
// trees, and the framework types the generated view/bootstrap modules
// name directly. g_type_closure holds that reachable set and
// in_projection() gates emission, forward-declaration, and dependency
// collection, so one set prunes every phase uniformly. Win32/WDK types
// are exempt — they are not type-pruned here (the module-level needed
// set already scopes them to what the generated code imports).
std::set<winmd::type_info const*> g_type_closure;
bool g_type_closure_active = false;

bool in_projection(winmd::type_info const& type)
{
    if (type.internal_)
    {
        return false;
    }
    if (type.ns == "Windows.Foundation.Metadata")
    {
        return false;
    }
    if (type.api_contract)
    {
        return false;
    }
    std::string full = type.full_name();
    if (!settings.include.empty() && !matches(settings.include, full))
    {
        return false;
    }
    if (!settings.exclude.empty() && matches(settings.exclude, full))
    {
        return false;
    }
    if (g_type_closure_active &&
        classify_namespace(type.ns) == ns_projection_model::winrt &&
        !g_type_closure.contains(&type))
    {
        return false;
    }
    return true;
}

// GUID formatting

std::string guid_literal(winmd::type_info const& type)
{
    char buffer[128];
    // the winmd stores the GUID as raw 16 bytes in blob order; the first
    // three fields (u32, u16, u16) must be assembled little-endian
    std::snprintf(buffer, sizeof(buffer),
        "winrt::winrt_guid{0x%02x%02x%02x%02x, 0x%02x%02x, 0x%02x%02x, {0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x, 0x%02x}}",
        type.guid[3], type.guid[2], type.guid[1], type.guid[0],
        type.guid[5], type.guid[4], type.guid[7], type.guid[6],
        type.guid[8], type.guid[9], type.guid[10], type.guid[11],
        type.guid[12], type.guid[13], type.guid[14], type.guid[15]);
    return buffer;
}

// ``{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}`` form used inside WinRT
// signatures (pinterface); lowercase hex, dashes, no braces ambiguity
std::string guid_text(std::uint8_t const (&g)[16])
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        g[3], g[2], g[1], g[0],
        g[5], g[4], g[7], g[6],
        g[8], g[9], g[10], g[11],
        g[12], g[13], g[14], g[15]);
    return buffer;
}

// signature element code for a primitive kind (i4/u8/f4/string/...)
std::string sig_element_primitive(std::uint8_t kind)
{
    switch (kind)
    {
    case winmd::et_boolean: return "b1";
    case winmd::et_char: return "c2";
    case winmd::et_i1: return "i1";
    case winmd::et_u1: return "u1";
    case winmd::et_i2: return "i2";
    case winmd::et_u2: return "u2";
    case winmd::et_i4: return "i4";
    case winmd::et_u4: return "u4";
    case winmd::et_i8: return "i8";
    case winmd::et_u8: return "u8";
    case winmd::et_r4: return "f4";
    case winmd::et_r8: return "f8";
    case winmd::et_string: return "string";
    default: return "object";
    }
}

// C++20 concepts: a requires clause from the metadata generic-parameter
// constraints (GenericParamAttributes special constraints and the
// GenericParamConstraint table). OS/SDK metadata strips constraint bits,
// so this stays empty for stock projections and only fires for user
// winmds that declare constraints — the 310-module output is unchanged.
std::string requires_clause(std::vector<winmd::generic_param_info> const& params, winmd::metadata const& metadata)
{
    std::string s;
    for (std::size_t i = 0; i < params.size(); ++i)
    {
        auto const& gp = params[i];
        std::string tn = "T" + std::to_string(i);
        if (gp.reference_type)
        {
            if (!s.empty()) s += " && ";
            s += "!std::is_arithmetic_v<" + tn + ">";
        }
        if (gp.not_nullable_value_type)
        {
            if (!s.empty()) s += " && ";
            s += "(std::is_arithmetic_v<" + tn + "> || std::is_enum_v<" + tn + ">)";
        }
        if (gp.default_ctor)
        {
            if (!s.empty()) s += " && ";
            s += "std::is_default_constructible_v<" + tn + ">";
        }
        for (auto const& c : gp.constraints)
        {
            if (!c.args.empty())
            {
                continue; // genericinst constraints stay unconstrained
            }
            auto const* ci = metadata.find(c);
            if (!ci || !(ci->is_interface() || ci->is_class()) || ci->generic_params.size())
            {
                continue;
            }
            if (!s.empty()) s += " && ";
            s += "std::is_convertible_v<" + tn + ", " + type_cpp_name(*ci) + ">";
        }
    }
    return s;
}

// template parameter list prefix for generic projected types ("", or
// "template <typename T0, typename T1>\n" with a requires clause when the
// metadata declares constraints)
std::string tpl_prefix(winmd::type_info const& type, winmd::metadata const& metadata)
{
    if (type.generic_params.empty())
    {
        return "";
    }
    std::string s = "template <";
    for (std::size_t i = 0; i < type.generic_params.size(); ++i)
    {
        if (i) s += ", ";
        s += "typename T" + std::to_string(i);
    }
    s += ">";
    std::string req = requires_clause(type.generic_params, metadata);
    if (!req.empty())
    {
        s += "\n    requires " + req;
    }
    s += "\n";
    return s;
}

// template argument list for a generic projected type ("", or "T0, T1")
std::string tpl_args(winmd::type_info const& type)
{
    std::string s;
    for (std::size_t i = 0; i < type.generic_params.size(); ++i)
    {
        if (i) s += ", ";
        s += "T" + std::to_string(i);
    }
    return s;
}

// flattened method list (base interface methods first)
std::vector<winmd::method_info const*> flattened_methods(winmd::type_info const& type, winmd::metadata const& metadata)
{
    std::vector<winmd::method_info const*> result;
    if (type.base && type.base->is_interface())
    {
        result = flattened_methods(*type.base, metadata);
    }
    for (auto const& method : type.methods)
    {
        result.push_back(&method);
    }
    return result;
}

// IAsyncInfo is listed in the metadata as an interface of the async
// interfaces, but it is NOT part of their vtable: the OS reaches it only
// by QI, so its five methods are never a vtable prefix.
bool is_async_info(winmd::type_info const& t)
{
    return is_foundation_type(t.ns, t.name) && t.name == "IAsyncInfo";
}

// Total methods in an interface's Extends-base chain (unique), which is
// the vtable prefix the OS lays out before the interface's own methods.
// Only the Extends link prefixes the vtable (mirroring flattened_methods);
// interfaces listed in the InterfaceImpl table — IAsyncInfo on the async
// interfaces, IClosable/IInputStream/IOutputStream on the streams — are
// reached by QI only and are never vtable prefixes, so they are not
// counted. Shared ancestors are counted once, so the dedup set is threaded
// through the recursion. IAsyncInfo is additionally excluded: its methods
// are never prefixed (see is_async_info).
std::size_t chain_method_count(winmd::type_info const& iface, std::set<winmd::type_info const*>& seen)
{
    std::size_t n = 0;
    auto const* b = iface.base;
    if (b && b->is_interface() && !is_async_info(*b) && seen.insert(b).second)
    {
        n += b->methods.size() + chain_method_count(*b, seen);
    }
    return n;
}

// The interface in the transitive base chain whose own TypeDef declares
// the method (matched by name and parameter count); null if the method is
// not declared by any base (i.e. it is the type's own method).
winmd::type_info const* find_declaring_in(winmd::type_info const& iface, winmd::method_info const& method)
{
    for (auto const& mm : iface.methods)
    {
        if (mm.name == method.name && mm.params.size() == method.params.size())
        {
            return &iface;
        }
    }
    for (auto const* b : iface.interfaces)
    {
        if (!b || !b->is_interface())
        {
            continue;
        }
        if (auto const* d = find_declaring_in(*b, method))
        {
            return d;
        }
    }
    return nullptr;
}

// The interface that declares a method on the projected wrapper: the
// declaring base in the chain, or the type itself when no base declares it
// (the metadata re-declares inherited methods on the derived TypeDef, so
// "own" means "not declared by any base").
winmd::type_info const* find_declaring(winmd::type_info const& type, winmd::method_info const& method)
{
    for (auto const* b : type.interfaces)
    {
        if (!b || !b->is_interface())
        {
            continue;
        }
        if (auto const* d = find_declaring_in(*b, method))
        {
            return d;
        }
    }
    return &type;
}

// Index of the method within the declaring interface's own TypeDef list.
std::size_t own_method_index(winmd::type_info const& decl, winmd::method_info const& method)
{
    for (std::size_t i = 0; i < decl.methods.size(); ++i)
    {
        if (decl.methods[i].name == method.name && decl.methods[i].params.size() == method.params.size())
        {
            return i;
        }
    }
    return 0;
}

// Namespaces referenced by a signature type (resolvable type rows only).
void collect_ref_ns(winmd::type const& t, winmd::metadata const& metadata, std::set<std::string, std::less<>>& out)
{
    if (t.kind == winmd::et_class || t.kind == winmd::et_valuetype || t.kind == winmd::et_genericinst)
    {
        if (auto const* type = metadata.find(t))
        {
            if (!type->ns.empty())
            {
                out.insert(type->ns);
            }
        }
        else
        {
            // Row-based fallback: the type exists in metadata but is not a
            // loaded type_info (its row still names ns+name). Without this,
            // every named struct/enum/interface reference (GUID -> GUIDDEF,
            // NODE_ADDRESS -> ...) silently lost its namespace, the owning
            // module never entered dep_owners, and the `export import` line
            // was skipped — C3083 'must be a type' in 157+ modules.
            std::string rns, rname;
            if (metadata.resolve_type_name(t, rns, rname) && !rns.empty())
            {
                out.insert(rns);
            }
            else if (std::getenv("CPPWINC_DEPTRACE"))
            {
                std::fprintf(stderr, "[dep] unresolvable sig ref kind=0x%02x\n", t.kind);
            }
        }
    }
    for (auto const& a : t.args)
    {
        collect_ref_ns(a, metadata, out);
    }
    for (auto const& c : t.children)
    {
        collect_ref_ns(c, metadata, out);
    }
}

void collect_method_ref_ns(winmd::method_info const& method, winmd::metadata const& metadata, std::set<std::string, std::less<>>& out)
{
    collect_ref_ns(method.ret, metadata, out);
    for (auto const& p : method.params)
    {
        collect_ref_ns(p.value, metadata, out);
    }
}

// Namespaces whose modules a projected type's emission depends on.
void collect_type_deps(winmd::type_info const& type, winmd::metadata const& metadata, std::set<std::string, std::less<>>& out)
{
    if (type.is_interface())
    {
        // inherited methods dispatch through a QI to the declaring base
        // interface, so the base's module must be imported (its iid_of
        // must be visible in this module)
        if (type.base && type.base->is_interface())
        {
            if (!type.base->ns.empty())
            {
                out.insert(type.base->ns);
            }
        }
        for (auto const* b : type.interfaces)
        {
            if (b && b->is_interface() && !b->ns.empty())
            {
                out.insert(b->ns);
            }
        }
        auto methods = flattened_methods(type, metadata);
        for (auto const* m : methods)
        {
            collect_method_ref_ns(*m, metadata, out);
        }
    }
    else if (type.is_class())
    {
        bool dep_dbg = std::getenv("CPPWINC_DEPTRACE") && type.name == "DesktopWindowTarget";
        if (dep_dbg) std::fprintf(stderr, "[dep] DesktopWindowTarget: ifaces=%zu statics=%zu methods=%zu\n", type.interfaces.size(), type.statics.size(), type.methods.size());
        for (auto const* iface : type.interfaces)
        {
            if (!iface)
            {
                continue;
            }
            if (dep_dbg) std::fprintf(stderr, "[dep] DesktopWindowTarget iface: %s.%s (is_iface=%d methods=%zu)\n", iface->ns.c_str(), iface->name.c_str(), (int)iface->is_interface(), iface->methods.size());
            if (!iface->ns.empty())
            {
                out.insert(iface->ns);
            }
            if (iface->is_interface())
            {
                auto methods = flattened_methods(*iface, metadata);
                for (auto const* m : methods)
                {
                    collect_method_ref_ns(*m, metadata, out);
                }
            }
        }
        for (auto const* statics : type.statics)
        {
            if (!statics)
            {
                continue;
            }
            if (!statics->ns.empty())
            {
                out.insert(statics->ns);
            }
            auto methods = flattened_methods(*statics, metadata);
            for (auto const* m : methods)
            {
                collect_method_ref_ns(*m, metadata, out);
            }
        }
        for (auto const& m : type.methods)
        {
            collect_method_ref_ns(m, metadata, out);
        }
        // Inherited instance methods: the class writer walks the base chain
        // and emits every base-interface method on the derived class, so the
        // modules owning those methods (and their signature types) must be
        // imported here too. Skipping this leaves classes whose base lives in
        // another namespace (e.g. DesktopWindowTarget <- ICompositionObject)
        // without the import for the inherited signatures' namespaces.
        winmd::type_info const* base = type.base;
        std::set<winmd::type_info const*> dep_seen_bases;
        while (base && dep_seen_bases.insert(base).second)
        {
            if (!base->ns.empty())
            {
                out.insert(base->ns);
            }
            for (auto const* base_iface : base->interfaces)
            {
                if (!base_iface || !base_iface->is_interface())
                {
                    continue;
                }
                if (!base_iface->ns.empty())
                {
                    out.insert(base_iface->ns);
                }
                auto base_methods = flattened_methods(*base_iface, metadata);
                for (auto const* m : base_methods)
                {
                    collect_method_ref_ns(*m, metadata, out);
                }
            }
            base = base->base;
        }
    }
    else if (type.is_delegate() && type.invoke >= 0)
    {
        collect_method_ref_ns(type.methods[type.invoke], metadata, out);
    }
    else if (type.is_struct())
    {
        for (auto const& f : type.fields)
        {
            collect_ref_ns(f.value, metadata, out);
        }
    }
}

void collect_namespace_deps(winmd::namespace_info const& info, winmd::metadata const& metadata, std::set<std::string, std::less<>>& out)
{
    auto add = [&](std::vector<winmd::type_info const*> const& types)
    {
        for (auto const* t : types)
        {
            if (in_projection(*t))
            {
                collect_type_deps(*t, metadata, out);
            }
        }
    };
    add(info.enums);
    add(info.structs);
    add(info.delegates);
    add(info.interfaces);
    add(info.classes);
}

// type-level dependency closure (selective -winui mode)
// Collects every type directly referenced by a type's metadata: base,
// interfaces, statics/activatables, method signatures, fields, properties,
// events, generic params/constraints. Returns types, not namespaces —
// namespace-granular deps (collect_type_deps) over-approximate: one used
// Grid drags the deps of ALL Xaml.Controls types into the module set.
void collect_type_edges(winmd::type_info const& type, winmd::metadata const& metadata,
    std::set<winmd::type_info const*>& out)
{
    auto add_type = [&](winmd::type_info const* t)
    {
        if (t && !t->ns.empty() && out.insert(t).second)
        {
            // closures over Windows.Foundation.Metadata are attribute-only;
            // the pruned in_projection already rejects them
        }
    };
    // Resolves every named type inside a signature type. Generic
    // instantiations encode their top-level row as a TypeSpec with the real
    // types inside `args`, and ptr/arr wrappers in `children` — metadata.find
    // sees neither, which previously pruned types like DeviceThumbnail out of
    // IAsyncOperation<DeviceThumbnail> and broke the emitting module.
    std::function<void(winmd::type const&)> add_sig = [&](winmd::type const& t)
    {
        if (auto const* r = metadata.find(t)) add_type(r);
        for (auto const& a : t.args) add_sig(a);
        for (auto const& c : t.children) add_sig(c);
        for (auto const& p : t.fn_params) add_sig(p);
        for (auto const& r0 : t.fn_ret) add_sig(r0);
    };
    if (type.base) add_type(type.base);
    for (auto const* i : type.interfaces) add_type(i);
    for (auto const* s : type.statics) add_type(s);
    for (auto const* a : type.activatable) add_type(a);
    add_type(type.default_interface);
    add_type(type.composable_factory);

    auto add_method = [&](winmd::method_info const& m)
    {
        add_sig(m.ret);
        for (auto const& p : m.params)
        {
            add_sig(p.value);
        }
    };
    for (auto const& m : type.methods) add_method(m);
    if (type.invoke >= 0 && type.invoke < static_cast<std::int32_t>(type.methods.size()))
        add_method(type.methods[static_cast<std::size_t>(type.invoke)]);
    for (auto const& f : type.fields)
    {
        add_sig(f.value);
    }
    for (auto const& p : type.properties)
    {
        add_sig(p.value);
        for (auto const& ip : p.index_params)
        {
            add_sig(ip.value);
        }
    }
    for (auto const& e : type.events)
    {
        // event types are stored as raw rows, not signature types
        if (auto const* t = metadata.find(e.type)) add_type(t);
    }
    for (auto const& gp : type.generic_params)
    {
        for (auto const& c : gp.constraints)
        {
            if (auto const* t = metadata.find(c)) add_type(t);
        }
    }
    // inherited-method dispatch: the class writer emits every base-interface
    // method on the class, so their signature types must close too
    winmd::type_info const* base = type.base;
    std::set<winmd::type_info const*> seen;
    while (base && seen.insert(base).second)
    {
        for (auto const* bi : base->interfaces)
        {
            if (!bi || !bi->is_interface()) continue;
            add_type(bi);
            for (auto const* m : flattened_methods(*bi, metadata))
                add_method(*m);
        }
        base = base->base;
    }
}

// selective -winui closure construction
// Type-level dependencies of one type, restricted to namespaces the
// -winui projection is actually emitting. collect_type_edges walks all
// metadata refs; winui_collect_edges keeps only winrt/WASDK namespaces
// (Win32/WDK types are scoped by the module-level needed set, and the
// app's own component namespaces seed the walk rather than being walked).
void winui_collect_edges(winmd::type_info const& type, winmd::metadata const& metadata,
    std::set<winmd::type_info const*>& out)
{
    std::set<winmd::type_info const*> raw;
    collect_type_edges(type, metadata, raw);
    for (auto const* t : raw)
    {
        if (classify_namespace(t->ns) != ns_projection_model::winrt) continue;
        out.insert(t);
    }
}

// Builds the reachable type set for -winui / -winui_no_compile mode.
// Seeds: the app's component classes plus every type the generated
// view/bootstrap code names via winrt:: namespaces (its imports are the
// ground truth for what the app compiles against). The BFS walks real
// metadata edges — base, interfaces, method/property/event signatures,
// fields — so one used Grid pulls only Grid's deps, not every sibling
// control's. Windows.Foundation/Metadata are framework substrate every
// consumer of the projection needs.
void build_winui_type_closure(winmd::metadata const& metadata,
    std::vector<winmd::type_info const*> const& component_roots,
    std::set<std::string, std::less<>> const& imported_ns)
{
    g_type_closure.clear();
    std::vector<winmd::type_info const*> work;
    auto enqueue = [&](winmd::type_info const* t)
    {
        if (!t || t->ns.empty()) return;
        if (classify_namespace(t->ns) != ns_projection_model::winrt) return;
        if (g_type_closure.insert(t).second)
        {
            work.push_back(t);
        }
    };
    for (auto const* t : component_roots) enqueue(t);
    for (auto const& ns : imported_ns)
    {
        if (!ns.starts_with("winrt.")) continue;
        std::string ns_name = ns.substr(6);
        if (ns_name == "Windows.Foundation" || ns_name == "Windows.Foundation.Metadata") continue;
        auto it = metadata.namespaces().find(ns_name);
        if (it == metadata.namespaces().end()) continue;
        auto const& info = it->second;
        for (auto const* t : info.classes) enqueue(t);
        for (auto const* t : info.enums) enqueue(t);
        for (auto const* t : info.structs) enqueue(t);
        for (auto const* t : info.delegates) enqueue(t);
        for (auto const* t : info.interfaces) enqueue(t);
    }
    // The hand-written async awaiters (write_async_awaiters) hard-code the
    // four Foundation completed-handler delegates for put_Completed on every
    // async shape. The edge walk can miss generic delegates, so seed them
    // explicitly — they are tiny and any async-using module needs them.
    if (auto it = metadata.namespaces().find("Windows.Foundation");
        it != metadata.namespaces().end())
    {
        for (char const* name : {
                "AsyncActionCompletedHandler",
                "AsyncActionWithProgressCompletedHandler",
                "AsyncOperationCompletedHandler",
                "AsyncOperationWithProgressCompletedHandler" })
        {
            for (auto const* d : it->second.delegates)
            {
                if (d->name == name) enqueue(d);
            }
        }
    }
    // The winuic view emitter hard-codes winrt::Microsoft::UI::Xaml::Documents::
    // Run for TextBlock.Text(...) and inline Span/Paragraph children, but the
    // app's own imports never name that namespace. Seed it — it is small and
    // every WinUI view with a TextBlock pulls it in.
    if (auto it = metadata.namespaces().find("Microsoft.UI.Xaml.Documents");
        it != metadata.namespaces().end())
    {
        for (auto const* t : it->second.classes) enqueue(t);
        for (auto const* t : it->second.enums) enqueue(t);
        for (auto const* t : it->second.structs) enqueue(t);
        for (auto const* t : it->second.delegates) enqueue(t);
        for (auto const* t : it->second.interfaces) enqueue(t);
    }
    while (!work.empty())
    {
        winmd::type_info const* t = work.back();
        work.pop_back();
        std::set<winmd::type_info const*> edges;
        winui_collect_edges(*t, metadata, edges);
        for (auto const* e : edges) enqueue(e);
    }
}

// module grouping (strongly connected components of the namespace
// dependency graph; cyclic namespaces merge into one module)

std::vector<std::string> split_dots(std::string const& s)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;)
    {
        auto dot = s.find('.', start);
        parts.push_back(dot == std::string::npos ? s.substr(start) : s.substr(start, dot - start));
        if (dot == std::string::npos)
        {
            break;
        }
        start = dot + 1;
    }
    return parts;
}

std::string common_ns_prefix(std::vector<std::string> const& names)
{
    std::vector<std::string> first = split_dots(names[0]);
    std::string result;
    for (std::size_t i = 0; i < first.size(); ++i)
    {
        bool all = true;
        for (auto const& n : names)
        {
            auto parts = split_dots(n);
            if (parts.size() <= i || parts[i] != first[i])
            {
                all = false;
                break;
            }
        }
        if (!all)
        {
            break;
        }
        result += (result.empty() ? "" : ".") + first[i];
    }
    return result;
}

struct scc_finder
{
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> const& deps;
    std::set<std::string, std::less<>> const& in_nodes;
    std::map<std::string, int, std::less<>> index;
    std::map<std::string, int, std::less<>> low;
    std::vector<std::string> stack;
    std::set<std::string, std::less<>> on_stack;
    std::vector<std::vector<std::string>> result;
    int counter = 0;

    void visit(std::string const& v)
    {
        index[v] = low[v] = counter++;
        stack.push_back(v);
        on_stack.insert(v);
        auto it = deps.find(v);
        if (it != deps.end())
        {
            for (auto const& w : it->second)
            {
                if (!in_nodes.contains(w))
                {
                    continue;
                }
                if (!index.contains(w))
                {
                    visit(w);
                    low[v] = (std::min)(low[v], low[w]);
                }
                else if (on_stack.contains(w))
                {
                    low[v] = (std::min)(low[v], index[w]);
                }
            }
        }
        if (low[v] == index[v])
        {
            std::vector<std::string> scc;
            for (;;)
            {
                std::string w = stack.back();
                stack.pop_back();
                on_stack.erase(w);
                scc.push_back(w);
                if (w == v)
                {
                    break;
                }
            }
            result.push_back(std::move(scc));
        }
    }
};

// Members of one module, ordered so that namespace-level dependencies
// come first; any residual cycle falls back to alphabetical order.
std::vector<std::string> order_group_members(std::vector<std::string> const& members,
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> const& deps)
{
    std::set<std::string, std::less<>> member_set(members.begin(), members.end());
    std::set<std::string, std::less<>> done;
    std::vector<std::string> result;
    for (;;)
    {
        std::string pick;
        bool picked = false;
        for (auto const& m : members)
        {
            if (done.contains(m))
            {
                continue;
            }
            bool ready = true;
            auto it = deps.find(m);
            if (it != deps.end())
            {
                for (auto const& d : it->second)
                {
                    if (d != m && member_set.contains(d) && !done.contains(d))
                    {
                        ready = false;
                        break;
                    }
                }
            }
            if (ready)
            {
                pick = m;
                picked = true;
                break;
            }
        }
        if (!picked)
        {
            break;
        }
        done.insert(pick);
        result.push_back(pick);
    }
    std::vector<std::string> leftover;
    for (auto const& m : members)
    {
        if (!done.contains(m))
        {
            leftover.push_back(m);
        }
    }
    // Keep leftovers in their original (declaration) order instead of
    // alphabetical — alphabetical sorting can place a namespace that
    // references types from another namespace before that namespace,
    // causing forward-reference errors in the generated code.
    result.insert(result.end(), leftover.begin(), leftover.end());
    return result;
}

