// cppwinc_proj.ixx — ABI helpers, projection writers
// Split from cppwinc.ixx for readability.
// This file is #included from cppwinc.ixx, not compiled independently.

// ABI call emission helpers

// C++ param declaration
// WinRT "out" parameters (metadata param_out flag) are written by the
// callee through a pointer; the projected signature takes a non-const
// reference so the caller's variable is updated, and the ABI call passes
// its address. Everything else travels by const& (value or interface).
std::string param_decl(winmd::type const& t, std::string const& name, type_mapper const& mapper, bool is_out)
{
    if (t.kind == winmd::et_szarray)
    {
        // array parameters use winrt::span (the array_view replacement)
        // so callers can pass std::vector, C arrays, com_array or other
        // span-convertible buffers
        return "winrt::span<" + mapper.map(t.children.at(0)) + "> const& " + name;
    }
    return mapper.map(t) + (is_out ? "& " : " const& ") + name;
}

// ABI expression for passing an argument; out parameters are written
// through a pointer, everything else is an in value
std::string abi_arg(winmd::type const& t, std::string const& arg, winmd::metadata const& metadata, bool is_out)
{
    switch (t.kind)
    {
    case winmd::et_boolean:
    case winmd::et_char:
    case winmd::et_i1:
    case winmd::et_u1:
    case winmd::et_i2:
    case winmd::et_u2:
    case winmd::et_i4:
    case winmd::et_u4:
    case winmd::et_i8:
    case winmd::et_u8:
    case winmd::et_r4:
    case winmd::et_r8:
        return is_out ? "&" + arg : arg;
    case winmd::et_string:
        // HSTRING* is void**: put_abi returns the slot address as void*,
        // typed up here to match the ABI fn type
        return is_out ? "reinterpret_cast<void**>(winrt::impl::put_abi(" + arg + "))" : arg + ".get_abi()";
    case winmd::et_szarray:
        // WinRT array ABI slots are (length, data); the projected param is
        // a span, so the call passes size() first, then data()
        return "static_cast<std::uint32_t>(" + arg + ".size()), " + arg + ".data()";
    case winmd::et_object:
    case winmd::et_genericinst:
        return is_out ? "reinterpret_cast<void**>(" + arg + ".put())" : "reinterpret_cast<void*>(" + arg + ".get_abi())";
    case winmd::et_class:
    {
        auto* type = metadata.find(t);
        if (type && is_foundation_type(type->ns, type->name) && type->name == "IInspectable")
        {
            // wrapper by value — its ABI is the raw pointer
            return is_out ? "reinterpret_cast<void**>(" + arg + ".put())" : arg;
        }
        if (type && !type->is_interface() && !type->is_delegate())
        {
            if (is_out)
                return "reinterpret_cast<void**>(" + arg + ".m_unknown.put())";
            // WinRT object params must travel as the EXACT interface subobject
            // the ABI signature declares. XAML's statics do
            // static_cast<DirectUI::FrameworkElement*>(pElement) on the raw
            // pointer (dxaml GridFactory::SetRowSpanStatic & friends), so
            // handing over the IUnknown/default subobject of a different
            // interface reads garbage fields — AV inside XAML.
            // QI the class to its default interface at the call boundary;
            // fall back to the unknown pointer when no default interface is
            // known (aggregated/local objects: same subobject, no QI needed).
            if (type->default_interface)
            {
                // Null-guard at the ABI boundary: an empty projected wrapper
                // (null m_unknown) QIs through a null pointer and fail-fasts
                // the process (XAML's fail-fast handler turns the AV into
                // 0xC000027B). A null object argument is meaningless anyway;
                // make it a benign no-op pointer instead of a crash.
                return "reinterpret_cast<void*>([&]{ if (!" + arg + ".m_unknown.get()) return static_cast<void*>(nullptr); void* qi_p = nullptr; winrt::check_hresult(winrt::impl::query_interface(" + arg + ".m_unknown.get(), winrt::iid_of<" + type_cpp_name(*type->default_interface) + ">, &qi_p)); return qi_p; }())";
            }
            return "reinterpret_cast<void*>(" + arg + ".m_unknown.get())";
        }
        return is_out ? "reinterpret_cast<void**>(" + arg + ".put())" : "reinterpret_cast<void*>(" + arg + ".get_abi())";
    }
    case winmd::et_var:
    case winmd::et_mvar:
        return is_out ? "winrt::impl::put_abi(" + arg + ")" : "winrt::impl::bind(" + arg + ")";
    case winmd::et_valuetype:
    {
        auto* type = metadata.find(t);
        if (type && type->is_enum())
        {
            return is_out ? "reinterpret_cast<std::int32_t*>(&" + arg + ")" : "static_cast<int32_t>(" + arg + ")";
        }
        // value type by value — the compiler applies the x64 convention
        // (<=16 bytes in registers, larger by hidden reference)
        return is_out ? "reinterpret_cast<void*>(&" + arg + ")" : arg;
    }
    default:
        return "reinterpret_cast<void*>(&" + arg + ")";
    }
}

// out-arg list for a return value
std::string ret_out(winmd::type const& t, std::string const& var, winmd::metadata const& metadata)
{
    if (t.kind == winmd::et_void)
    {
        return {};
    }
    switch (t.kind)
    {
    case winmd::et_var:
    case winmd::et_mvar:
        return ", winrt::impl::put_abi(" + var + ")";
    case winmd::et_boolean:
    case winmd::et_char:
    case winmd::et_i1:
    case winmd::et_u1:
    case winmd::et_i2:
    case winmd::et_u2:
    case winmd::et_i4:
    case winmd::et_u4:
    case winmd::et_i8:
    case winmd::et_u8:
    case winmd::et_r4:
    case winmd::et_r8:
        return ", &" + var;
    case winmd::et_szarray:
        return ", &" + var + ".m_size, reinterpret_cast<void**>(" + var + ".put())";
    case winmd::et_valuetype:
    {
        auto* type = metadata.find(t);
        if (type && type->is_enum())
        {
            return ", reinterpret_cast<std::int32_t*>(&" + var + ")";
        }
        return ", reinterpret_cast<void*>(&" + var + ")";
    }
    default:
        return ", &" + var + "_abi";
    }
}

// ABI slot type for a parameter; out parameters are pointers (the
// callee writes through them), everything else travels by value
std::string abi_param_type(winmd::type const& t, type_mapper const& mapper, winmd::metadata const& metadata, bool is_out)
{
    if (t.kind == winmd::et_var || t.kind == winmd::et_mvar)
    {
        return is_out ? "winrt::impl::put_abi_t<T" + std::to_string(t.var) + ">" : "winrt::impl::arg_in_t<T" + std::to_string(t.var) + ">";
    }
    if (t.kind == winmd::et_szarray)
    {
        return "std::uint32_t, void*";
    }
    if (t.kind == winmd::et_valuetype)
    {
        auto* type = metadata.find(t);
        if (type && type->is_enum())
        {
            return is_out ? "std::int32_t*" : "std::int32_t";
        }
        // value types travel by value in the ABI exactly as the OS
        // expects: structs <= 16 bytes in registers, larger ones by
        // reference — the compiler picks the right convention from the
        // projected type alone; out structs go by pointer
        return is_out ? "void*" : mapper.map(t);
    }
    if (t.kind == winmd::et_class)
    {
        // Windows.Foundation.IInspectable is the projected wrapper whose
        // ABI layout is the single raw pointer — travel by value
        auto* type = metadata.find(t);
        if (type && is_foundation_type(type->ns, type->name) && type->name == "IInspectable")
        {
            return is_out ? "void**" : "winrt::IInspectable";
        }
        return is_out ? "void**" : "void*";
    }
    if (t.kind >= winmd::et_boolean && t.kind <= winmd::et_r8 && t.kind != winmd::et_string)
    {
        return primitive_cpp(t.kind) + (is_out ? "*" : "");
    }
    return is_out ? "void**" : "void*";
}

// ABI slot type for the retval out-parameter (WinRT HRESULT return conventions)
std::string ret_out_type(winmd::type const& t, winmd::metadata const& metadata)
{
    if (t.kind == winmd::et_var || t.kind == winmd::et_mvar)
    {
        return "winrt::impl::put_abi_t<T" + std::to_string(t.var) + ">";
    }
    if (t.kind == winmd::et_szarray)
    {
        return "std::uint32_t*, void**";
    }
    if (t.kind == winmd::et_valuetype)
    {
        auto* type = metadata.find(t);
        if (type && type->is_enum())
        {
            return "std::int32_t*";
        }
        return "void*";
    }
    if (t.kind >= winmd::et_boolean && t.kind <= winmd::et_r8 && t.kind != winmd::et_string)
    {
        return primitive_cpp(t.kind) + "*";
    }
    return "void**";
}

// Complete ABI signature for a vtable slot: WinRT ABI methods always
// return HRESULT (int32_t); the projected return is a trailing out-param.
std::string abi_fn_type(winmd::method_info const& method, type_mapper const& mapper, winmd::metadata const& metadata)
{
    std::string s = "std::int32_t(__stdcall*)(void*";
    for (auto const& p : method.params)
    {
        s += ", " + abi_param_type(p.value, mapper, metadata, (p.flags & winmd::param_out) != 0);
    }
    if (method.ret.kind != winmd::et_void)
    {
        s += ", " + ret_out_type(method.ret, metadata);
    }
    s += ")";
    return s;
}

// declared return type
std::string ret_type(winmd::type const& t, type_mapper const& mapper)
{
    if (t.kind == winmd::et_void)
    {
        return "void";
    }
    return mapper.map(t);
}

// return conversion statement
std::string ret_convert(winmd::type const& t, std::string const& var, type_mapper const& mapper, winmd::metadata const& metadata)
{
    if (t.kind == winmd::et_void)
    {
        return {};
    }
    switch (t.kind)
    {
    case winmd::et_var:
    case winmd::et_mvar:
        return "return " + var + ";";
    case winmd::et_boolean:
    case winmd::et_char:
    case winmd::et_i1:
    case winmd::et_u1:
    case winmd::et_i2:
    case winmd::et_u2:
    case winmd::et_i4:
    case winmd::et_u4:
    case winmd::et_i8:
    case winmd::et_u8:
    case winmd::et_r4:
    case winmd::et_r8:
    case winmd::et_szarray:
        return "return " + var + ";";
    case winmd::et_valuetype:
    {
        auto* type = metadata.find(t);
        if (type && type->is_enum())
        {
            return "return static_cast<" + mapper.map(t) + ">(" + var + ");";
        }
        return "return " + var + ";";
    }
    default:
        return "return " + mapper.map(t) + "::from_abi(" + var + "_abi);";
    }
}

std::string var_decl(winmd::type const& t, std::string const& var, type_mapper const& mapper, winmd::metadata const& metadata)
{
    if (t.kind == winmd::et_void)
    {
        return {};
    }
    switch (t.kind)
    {
    case winmd::et_var:
    case winmd::et_mvar:
        return "    " + mapper.map(t) + " " + var + "{};\n";
    case winmd::et_boolean:
    case winmd::et_char:
    case winmd::et_i1:
    case winmd::et_u1:
    case winmd::et_i2:
    case winmd::et_u2:
    case winmd::et_i4:
    case winmd::et_u4:
    case winmd::et_i8:
    case winmd::et_u8:
    case winmd::et_r4:
    case winmd::et_r8:
    case winmd::et_szarray:
        return "    " + mapper.map(t) + " " + var + "{};\n";
    case winmd::et_valuetype:
    {
        auto* type = metadata.find(t);
        if (type && type->is_enum())
        {
            return "    int32_t " + var + "{0};\n";
        }
        return "    " + mapper.map(t) + " " + var + "{};\n";
    }
    default:
        return "    void* " + var + "_abi = nullptr;\n";
    }
}

// member-function signature: "ret name(p0, p1)" (+ " const" when
// const_member).  When `qual` is non-empty the name is emitted as
// "qual::name" (for out-of-line definitions at module scope).
std::string method_sig(winmd::method_info const& method, type_mapper const& mapper, bool const_member, std::string const& qual = {})
{
    std::string s = ret_type(method.ret, mapper) + " " + (qual.empty() ? method.name : qual + "::" + method.name) + "(";
    for (std::size_t p = 0; p < method.params.size(); ++p)
    {
        if (p) s += ", ";
        std::string name = method.params[p].name.empty() ? "arg" + std::to_string(p) : method.params[p].name;
        s += param_decl(method.params[p].value, name, mapper, (method.params[p].flags & winmd::param_out) != 0);
    }
    s += ")";
    if (const_member) s += " const";
    return s;
}

// body of a vtable-call member function: "{ ... }\n"
std::string abi_call_body(winmd::method_info const& method, std::string const& this_expr, std::size_t slot,
    type_mapper const& mapper, winmd::metadata const& metadata, std::string const& indent)
{
    writer b;
    std::string call_args;
    for (std::size_t p = 0; p < method.params.size(); ++p)
    {
        if (p) call_args += ", ";
        std::string name = method.params[p].name.empty() ? "arg" + std::to_string(p) : method.params[p].name;
        call_args += abi_arg(method.params[p].value, name, metadata, (method.params[p].flags & winmd::param_out) != 0);
    }

    // the metadata may name a parameter "result" (e.g. TryParse) —
    // avoid colliding with the generated return-value variable
    std::string ret_var = "result";
    for (auto const& p : method.params)
    {
        if (p.name == ret_var)
        {
            ret_var += "_";
        }
    }
    std::string decl = var_decl(method.ret, ret_var, mapper, metadata);
    if (!decl.empty())
    {
        b.write("%", decl);
    }

    b.write("%    auto* vt = *reinterpret_cast<void* const**>(%);\n", indent, this_expr);
    std::string fn_type = abi_fn_type(method, mapper, metadata);
    b.write("%    auto fn = reinterpret_cast<%>(vt[%]);\n", indent, fn_type, slot);

    if (method.ret.kind == winmd::et_void)
    {
        b.write("%    winrt::check_hresult(fn(%));\n", indent, call_args.empty() ? this_expr : this_expr + ", " + call_args);
    }
    else
    {
        std::string outs = ret_out(method.ret, ret_var, metadata);
        b.write("%    winrt::check_hresult(fn(%));\n", indent, call_args.empty() ? this_expr + outs : this_expr + ", " + call_args + outs);
        b.write("%    %\n", indent, ret_convert(method.ret, ret_var, mapper, metadata));
    }
    return b.str();
}

// out-of-line member definition text (templated prefix + signature + body)
std::string out_of_line(winmd::type_info const& type, winmd::metadata const& metadata, std::string const& sig, std::string const& body)
{
    return tpl_prefix(type, metadata) + "inline " + sig + "\n{\n" + body + "}\n\n";
}

// projection writers

void write_module_preamble(writer& w, std::string_view module_name)
{
    w.write("// %\n", module_name);
    w.write("\n");
    w.write("module;\n\n");
    // Header-free: no COM headers are included. IUnknown/IInspectable and
    // every Win32/COM API come from winrt_base (hand-laid ABI + winversec
    // module imports), and the standard library comes from `import std;`,
    // which winrt_base re-exports (`export import std;`), so every module
    // in the projection shares one parsed copy of the standard library.
    w.write("#include <cstdint>\n\n");
    w.write("export module %;\n\n", module_name);
    w.write("import std;\n\n");
    w.write("export import winrt_base;\n\n");
}

// Win32/WDK module preamble: self-contained plain C++ types.
// The header comment names the projection surface that produced the module.
void write_win32_wdk_module_preamble(writer& w, std::string_view module_name, bool is_wdk)
{
    w.write("// %\n", module_name);
    w.write("// CppWinC % projection\n", is_wdk ? "WDK" : "Win32");
    w.write("\n");
    w.write("module;\n\n");
    // Never include <windows.h>: the projected APIs are self-contained
    // (every type maps to void*/stdint and constants are emitted inline),
    // and including it would redeclare the very APIs being projected
    // (CloseHandle, IsDebuggerPresent, ...) with conflicting signatures.
    w.write("#include <cstdint>\n");
    w.write("#include <string>\n");
    w.write("#include <string_view>\n");
    w.write("#include <vector>\n");
    w.write("#include <functional>\n\n");
    w.write("export module %;\n\n", module_name);
    w.write("import std;\n\n");
}

// WinRT signature element for a metadata type (the `i4`/`u8`/`f4` codes
// used by type_signature<...> and the `pinterface`/`enum`/`struct` forms)
std::string sig_element(winmd::type const& t, winmd::metadata const& metadata)
{
    switch (t.kind)
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
    case winmd::et_object: return "object";
    case winmd::et_valuetype:
    case winmd::et_class:
    case winmd::et_genericinst:
    {
        auto const* type = metadata.find(t);
        if (!type) return "object";
        if (type->is_enum())
        {
            return "enum(" + type->full_name() + ";" + sig_element_primitive(type->enum_underlying) + ")";
        }
        if (type->is_struct())
        {
            std::string s = "struct(" + type->full_name();
            for (auto const& f : type->fields)
            {
                s += ";" + sig_element(f.value, metadata);
            }
            return s + ")";
        }
        // interface (generic or not): pinterface({pattern-guid};args...)
        std::string s = "pinterface({";
        s += guid_text(type->guid);
        s += "}";
        if (t.kind == winmd::et_genericinst)
        {
            for (auto const& a : t.args)
            {
                s += ";" + sig_element(a, metadata);
            }
        }
        return s + ")";
    }
    default:
        return "object";
    }
}

// whether a signature for this type would embed `object` (an
// IInspectable field — directly or through a nested struct)
bool sig_has_object(winmd::type const& t, winmd::metadata const& metadata)
{
    if (t.kind == winmd::et_object)
    {
        return true;
    }
    if (t.kind == winmd::et_valuetype || t.kind == winmd::et_class)
    {
        auto const* type = metadata.find(t);
        if (!type)
        {
            // an unresolved valuetype/class renders as `object` in the
            // signature (e.g. an IInspectable-typed field)
            return true;
        }
        if (is_foundation_type(type->ns, type->name) && type->name == "IInspectable")
        {
            return true;
        }
        if (type->is_struct())
        {
            for (auto const& f : type->fields)
            {
                if (sig_has_object(f.value, metadata)) return true;
            }
        }
    }
    if (t.kind == winmd::et_genericinst)
    {
        for (auto const& a : t.args)
        {
            if (sig_has_object(a, metadata)) return true;
        }
    }
    return false;
}

void write_enum(writer& w, winmd::type_info const& type, type_mapper const&, std::string& winrt_specs)
{
    std::string underlying = enum_underlying_cpp(type.enum_underlying);
    if (underlying.empty()) underlying = "std::int32_t";
    w.write("export enum class % : %\n{\n", type.name, underlying);
    for (auto const& [name, value] : type.enum_fields)
    {
        w.write("    % = %,\n", name, value);
    }
    w.write("};\n\n");
    winrt_specs += "template <> inline constexpr std::string_view type_signature<" + type_cpp_name(type) + "> = \"enum(" + type.full_name() + ";" + sig_element_primitive(type.enum_underlying) + ")\";\n";
}

void write_struct(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const& metadata, std::string& winrt_specs)
{
    w.write("export struct %\n{\n", type.name);
    for (auto const& field : type.fields)
    {
        w.write("    % %;\n", mapper.map(field.value), field.name);
    }
    w.write("    friend auto operator<=>(% const&, % const&) = default;\n", type.name, type.name);
    w.write("};\n\n");
    std::string sig = "struct(" + type.full_name();
    bool object_field = false;
    for (auto const& f : type.fields)
    {
        // a struct whose signature would embed `object` (an IInspectable
        // field, directly or nested) gets no type_signature at all
        if (sig_has_object(f.value, metadata)) object_field = true;
        sig += ";" + sig_element(f.value, metadata);
    }
    sig += ")";
    if (!object_field)
    {
        winrt_specs += "template <> inline constexpr std::string_view type_signature<" + type_cpp_name(type) + "> = \"" + sig + "\";\n";
    }
}

// the four async protocols get coroutine support: a blocking get() member
// and an operator co_await that constructs the shared awaiter from the
// raw interface pointer

enum class async_shape
{
    none,
    action,
    action_progress,
    operation,
    operation_progress,
};

async_shape async_shape_of(winmd::type_info const& type)
{
    // the reader stores generic names without the arity suffix
    std::string name = std::string(type.ns) + "." + type.name;
    // Handle Windows.Foundation (SDK) and Microsoft.Windows.Foundation (WASDK 2.3+)
    if (name == "Windows.Foundation.IAsyncAction" ||
        name == "Microsoft.Windows.Foundation.IAsyncAction") return async_shape::action;
    if (name == "Windows.Foundation.IAsyncActionWithProgress" ||
        name == "Microsoft.Windows.Foundation.IAsyncActionWithProgress") return async_shape::action_progress;
    if (name == "Windows.Foundation.IAsyncOperation" ||
        name == "Microsoft.Windows.Foundation.IAsyncOperation") return async_shape::operation;
    if (name == "Windows.Foundation.IAsyncOperationWithProgress" ||
        name == "Microsoft.Windows.Foundation.IAsyncOperationWithProgress") return async_shape::operation_progress;
    return async_shape::none;
}

void write_async_awaiters(writer& w, std::string& async_helpers);

bool is_component_type(winmd::type_info const& type);

// A method whose signature references a component class (e.g. a statics
// factory method returning the component class) cannot have its body
// emitted in the projection module, where the class is only
// forward-declared. Such bodies are routed to the impl module, where the
// class is complete (see the component generation section).
bool method_refs_component(winmd::method_info const& method, winmd::metadata const& metadata)
{
    auto refs = [&](winmd::type const& t)
    {
        if (t.kind == winmd::et_class)
        {
            auto const* ti = metadata.find(t);
            return ti != nullptr && settings.component && is_component_type(*ti);
        }
        return false;
    };
    if (refs(method.ret)) return true;
    for (auto const& p : method.params)
    {
        if (refs(p.value)) return true;
    }
    return false;
}

void write_interface(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const& metadata,
    std::string& winrt_specs, std::string& bodies, std::string& component_bodies, std::mutex& component_bodies_mutex,
    bool& module_has_async, std::string& co_await_text)
{
    std::string cpp_name = type_cpp_name(type);
    bool generic = !type.generic_params.empty();
    std::string tpl = tpl_prefix(type, metadata);
    std::string targs = tpl_args(type);
    std::string qual = cpp_name + (generic ? "<" + targs + ">" : "");
    auto methods = flattened_methods(type, metadata);

    // Inherited methods cannot be dispatched through the derived
    // interface's vtable slot: OS objects may implement the derived
    // interface as a tear-off whose vtable does not contain the base
    // methods (e.g. Shell's FileRandomAccessStream). They must be
    // dispatched through a pointer obtained by QI to the base interface,
    // exactly like the official projection does. The winmd keeps the
    // base's vtable methods as a prefix of the derived type's method list
    // (same order), so a name-matched prefix of the method list is
    // dispatched on the QI'd base pointer at slot 6 + i.
    // resolve each method's declaring interface and its absolute vtable
    // slot. WinRT vtables are prefix-laid: an interface's own TypeDef list
    // re-declares inherited methods, so each method must dispatch on the
    // interface that declares it (QI'd, since OS tear-offs may not share
    // the derived vtable) at 6 + (declaring chain prefix) + (own index).
    std::vector<std::pair<winmd::type_info const*, std::size_t>> locations;
    locations.reserve(methods.size());
    for (auto const* mp : methods)
    {
        auto const& method = *mp;
        auto const* decl = find_declaring(type, method);
        std::set<winmd::type_info const*> seen;
        std::size_t slot = 6 + chain_method_count(*decl, seen) + own_method_index(*decl, method);
        locations.emplace_back(decl, slot);
    }
    async_shape shape = async_shape_of(type);

    if (generic)
    {
        // generic interfaces have no static iid literal; the instance iid
        // is computed at runtime (MakeIID) from the generic arguments
        w.write("export %struct %;\n", tpl, type.name);
        winrt_specs += tpl_prefix(type, metadata) + "inline constexpr std::size_t interface_method_count<" + cpp_name + "<" + targs + ">> = " + std::to_string(methods.size()) + ";\n";
    }
    else
    {
        w.write("export struct %;\n", type.name);
        winrt_specs += "template <> inline constexpr winrt_guid iid_of<" + cpp_name + "> = " + guid_literal(type) + ";\n";
        winrt_specs += "template <> inline constexpr std::size_t interface_method_count<" + cpp_name + "> = " + std::to_string(methods.size()) + ";\n";
    }

    w.write("export %struct %\n{\n", tpl, type.name);
    // IReference<T> gains a boxing constructor (mirrors official cppwinrt:
    // IReference(T const&) via impl::reference<T>) so value types the
    // PropertyValue statics cannot create (Color, FontWeight, ...) can be
    // constructed directly for nullable property setters.
    if (type.ns == "Windows.Foundation" && type.name == "IReference")
    {
        w.write(R"cppwinc(    void* m_ptr = nullptr;

%() noexcept = default;
%(void* ptr) noexcept : m_ptr(ptr) {}
%(T0 const& value) : m_ptr(winrt::impl::make_reference_box(value)) {}
%(% const& other) : m_ptr(other.m_ptr) { if (m_ptr) IUnknown::addref_raw(m_ptr); }
%(%&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }
%& operator=(% const& other) { if (this != &other) { % copy(other); std::swap(m_ptr, copy.m_ptr); } return *this; }
%& operator=(%&& other) noexcept { if (this != &other) { m_ptr = other.m_ptr; other.m_ptr = nullptr; } return *this; }
~%() { if (m_ptr) IUnknown::release_raw(m_ptr); }
void* get_abi() const noexcept { return m_ptr; }
void** put() noexcept { if (m_ptr) IUnknown::release_raw(m_ptr); m_ptr = nullptr; return &m_ptr; }
static % from_abi(void* ptr) noexcept { return %(ptr); }
explicit operator bool() const noexcept { return m_ptr != nullptr; }
friend auto operator<=>(% const&, % const&) = default;
bool operator==(std::nullptr_t) const noexcept { return m_ptr == nullptr; }
)cppwinc",
        type.name,
        type.name, type.name,
        type.name, type.name,
        type.name, type.name, type.name,
        type.name, type.name, type.name,
        type.name, type.name,
        type.name, type.name,
        type.name, type.name, type.name);
    }
    else
    {
        w.write(R"cppwinc(    void* m_ptr = nullptr;

%() noexcept = default;
%(void* ptr) noexcept : m_ptr(ptr) {}
%(% const& other) : m_ptr(other.m_ptr) { if (m_ptr) IUnknown::addref_raw(m_ptr); }
%(%&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }
%& operator=(% const& other) { if (this != &other) { % copy(other); std::swap(m_ptr, copy.m_ptr); } return *this; }
%& operator=(%&& other) noexcept { if (this != &other) { m_ptr = other.m_ptr; other.m_ptr = nullptr; } return *this; }
~%() { if (m_ptr) IUnknown::release_raw(m_ptr); }
void* get_abi() const noexcept { return m_ptr; }
void** put() noexcept { if (m_ptr) IUnknown::release_raw(m_ptr); m_ptr = nullptr; return &m_ptr; }
static % from_abi(void* ptr) noexcept { return %(ptr); }
explicit operator bool() const noexcept { return m_ptr != nullptr; }
friend auto operator<=>(% const&, % const&) = default;
bool operator==(std::nullptr_t) const noexcept { return m_ptr == nullptr; }
)cppwinc",
        type.name,
        type.name, type.name,
        type.name, type.name, type.name,
        type.name, type.name, type.name,
        type.name, type.name,
        type.name, type.name,
        type.name, type.name,
        type.name, type.name, type.name);
    }

    w.write("\n");
    if (generic)
    {
        // Instance IID for a closed generic instantiation, runtime-computed
        // exactly like the delegate iid() (the pinterface SHA-1 cannot be
        // constexpr): authoring-side query_interface compares against it,
        // and consumers acquire through impl::query_interface(ptr, iid(), ...).
        std::string gargs;
        for (std::size_t p = 0; p < type.generic_params.size(); ++p)
            gargs += (p ? ", " : "") + std::string("winrt::generic_arg_of<T") + std::to_string(p) + ">";
        w.write("    static winrt::winrt_guid const& iid() noexcept\n    {\n");
        w.write("        static winrt::winrt_guid const value = winrt::impl::make_generic_iid(% , %);\n", guid_literal(type), gargs);
        w.write("        return value;\n    }\n\n");
    }
    std::set<std::string> iface_emitted;
    for (std::size_t i = 0; i < methods.size(); ++i)
    {
        auto const& method = *methods[i];
        // Deduplicate: WinMD metadata may have two overloads with identical
        // C++ signature (same param types, different param names) — e.g.
        // Union(target, point) and Union(target, rect). Skip duplicates.
        {
            std::string mkey = method.name;
            for (auto const& p : method.params)
                mkey += "|" + mapper.map(p.value);
            if (!iface_emitted.insert(mkey).second) continue;
        }
        auto const* decl = locations[i].first;
        std::size_t slot = locations[i].second;
        std::string body;
        if (decl != &type)
        {
            // QI to the declaring interface, then dispatch on the QI'd
            // pointer at the slot within that interface's own vtable.
            writer q;
            std::string iid_expr;
            if (decl->generic_params.empty())
            {
                iid_expr = "winrt::iid_of<" + type_cpp_name(*decl) + ">";
            }
            else
            {
                // generic base (base args are the derived's params in
                // order — only such bases resolve to a type_info); the
                // instance IID is pattern + args via MakeIID
                std::string args;
                for (std::size_t p = 0; p < decl->generic_params.size(); ++p)
                {
                    if (p) args += ", ";
                    args += "winrt::generic_arg_of<T" + std::to_string(p) + ">";
                }
                iid_expr = "winrt::impl::make_generic_iid(" + guid_literal(*decl) + ", " + args + ")";
            }
            q.write("        winrt::com_ptr<void> base;\n");
            q.write("        winrt::check_hresult(winrt::impl::query_interface(m_ptr, %, base.put()));\n", iid_expr);
            q.write("%", abi_call_body(method, "base.get()", slot, mapper, metadata, "        "));
            body = q.str();
        }
        else
        {
            body = abi_call_body(method, "m_ptr", slot, mapper, metadata, "    ");
        }
        w.write("    %;\n", method_sig(method, mapper, true));
        if (settings.component && method_refs_component(method, metadata))
        {
            // the body needs the component class complete, which only the
            // impl module has; emit it there after the class definition
            std::lock_guard<std::mutex> guard(component_bodies_mutex);
            component_bodies += out_of_line(type, metadata, method_sig(method, mapper, true, qual), body);
        }
        else
        {
            bodies += out_of_line(type, metadata, method_sig(method, mapper, true, qual), body);
        }
    }

    if (shape != async_shape::none)
    {
        // Blocking get(): the wait state is shared with a fully typed
        // completed handler, because the OS put_Completed validates the
        // handler against the concrete Async*CompletedHandler delegate
        // IID (which for generic operations depends on the result type).
        std::string ret;
        std::string handler_name;
        std::string view_name;
        std::string res_expr;
        switch (shape)
        {
        case async_shape::action:
            ret = "void";
            handler_name = "AsyncActionCompletedHandler";
            view_name = "IAsyncAction";
            res_expr = "GetResults();";
            break;
        case async_shape::action_progress:
            ret = "void";
            handler_name = "AsyncActionWithProgressCompletedHandler<T0>";
            view_name = "IAsyncActionWithProgress<T0>";
            res_expr = "GetResults();";
            break;
        case async_shape::operation:
            ret = "T0";
            handler_name = "AsyncOperationCompletedHandler<T0>";
            view_name = "IAsyncOperation<T0>";
            res_expr = "return GetResults();";
            break;
        default:
            ret = "T0";
            handler_name = "AsyncOperationWithProgressCompletedHandler<T0, T1>";
            view_name = "IAsyncOperationWithProgress<T0, T1>";
            res_expr = "return GetResults();";
            break;
        }
        w.write(R"cppwinc(    % get() const
{
    winrt::impl::check_sta_blocking_wait();
    std::int32_t status = winrt::impl::status_of(m_ptr);
    if (status == winrt::impl::async_status_created || status == winrt::impl::async_status_started)
    {
        auto state = std::make_shared<winrt::impl::async_wait_state>();
        % handler{ [state](% const&, AsyncStatus status) noexcept { state->invoke(static_cast<std::int32_t>(status)); } };
        put_Completed(handler);
        status = state->wait();
    }
    if (status == winrt::impl::async_status_canceled)
    {
        throw winrt::hresult_canceled(winrt::hr_canceled);
    }
    if (status == winrt::impl::async_status_error)
    {
        winrt::check_hresult(winrt::impl::error_of(m_ptr));
    }
    %
}
)cppwinc", ret, handler_name, view_name, res_expr);
    }

    w.write("};\n\n");

    if (shape != async_shape::none)
    {
        std::string awaiter;
        switch (shape)
        {
        case async_shape::action:
            awaiter = "winrt::impl::async_action_awaiter";
            break;
        case async_shape::action_progress:
            awaiter = "winrt::impl::async_action_progress_awaiter<T0>";
            break;
        case async_shape::operation:
            awaiter = "winrt::impl::async_operation_awaiter<T0>";
            break;
        default:
            awaiter = "winrt::impl::async_operation_progress_awaiter<T0, T1>";
            break;
        }
        std::string param = generic ? qual : cpp_name;
        writer q;
        if (generic)
        {
            q.write("export %inline % operator co_await(% const& async) noexcept\n", tpl_prefix(type, metadata), awaiter, param);
        }
        else
        {
            q.write("export inline % operator co_await(% const& async) noexcept\n", awaiter, param);
        }
        q.write("{\n");
        q.write("    return %{ async };\n", awaiter);
        q.write("}\n\n");
        co_await_text += q.str();
        module_has_async = true;
    }
}

void write_async_awaiters(writer& w, std::string& async_helpers)
{
    // The awaiters must live in the module (not winrt_base): they attach
    // a fully typed completed handler whose delegate IID is concrete
    // (generic delegates resolve it via MakeIID from their type
    // arguments). The shared state they drive comes from winrt_base.
    w.write(R"cppwinc(
    struct async_action_awaiter
    {
        winrt::Windows::Foundation::IAsyncAction const& m_async;
        std::shared_ptr<winrt::impl::async_coroutine_state> m_state;
        std::int32_t m_error = winrt::hr_ok;

        explicit async_action_awaiter(winrt::Windows::Foundation::IAsyncAction const& async) noexcept : m_async(async) {}

        bool await_ready() const noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<void> handle)
        {
            auto state = std::make_shared<winrt::impl::async_coroutine_state>(&m_error);
            m_state = state;
            state->prepare(handle);
            winrt::Windows::Foundation::AsyncActionCompletedHandler handler
            {
                [state, async = m_async.get_abi()](
                    winrt::Windows::Foundation::IAsyncAction const&,
                    winrt::Windows::Foundation::AsyncStatus status) noexcept
                {
                    state->complete_status(static_cast<std::int32_t>(status), async);
                }
            };
            m_async.put_Completed(handler);
            return state->suspend();
        }

        void await_resume()
        {
            winrt::check_hresult(m_error);
            m_state.reset();
        }
    };

    template <typename T0>
    struct async_action_progress_awaiter
    {
        winrt::Windows::Foundation::IAsyncActionWithProgress<T0> const& m_async;
        std::shared_ptr<winrt::impl::async_coroutine_state> m_state;
        std::int32_t m_error = winrt::hr_ok;

        explicit async_action_progress_awaiter(winrt::Windows::Foundation::IAsyncActionWithProgress<T0> const& async) noexcept : m_async(async) {}

        bool await_ready() const noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<void> handle)
        {
            auto state = std::make_shared<winrt::impl::async_coroutine_state>(&m_error);
            m_state = state;
            state->prepare(handle);
            winrt::Windows::Foundation::AsyncActionWithProgressCompletedHandler<T0> handler
            {
                [state, async = m_async.get_abi()](
                    winrt::Windows::Foundation::IAsyncActionWithProgress<T0> const&,
                    winrt::Windows::Foundation::AsyncStatus status) noexcept
                {
                    state->complete_status(static_cast<std::int32_t>(status), async);
                }
            };
            m_async.put_Completed(handler);
            return state->suspend();
        }

        void await_resume()
        {
            winrt::check_hresult(m_error);
            m_state.reset();
        }
    };

    template <typename T0>
    struct async_operation_awaiter
    {
        winrt::Windows::Foundation::IAsyncOperation<T0> const& m_async;
        std::shared_ptr<winrt::impl::async_coroutine_state> m_state;
        std::int32_t m_error = winrt::hr_ok;

        explicit async_operation_awaiter(winrt::Windows::Foundation::IAsyncOperation<T0> const& async) noexcept : m_async(async) {}

        bool await_ready() const noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<void> handle)
        {
            auto state = std::make_shared<winrt::impl::async_coroutine_state>(&m_error);
            m_state = state;
            state->prepare(handle);
            winrt::Windows::Foundation::AsyncOperationCompletedHandler<T0> handler
            {
                [state, async = m_async.get_abi()](
                    winrt::Windows::Foundation::IAsyncOperation<T0> const&,
                    winrt::Windows::Foundation::AsyncStatus status) noexcept
                {
                    state->complete_status(static_cast<std::int32_t>(status), async);
                }
            };
            m_async.put_Completed(handler);
            return state->suspend();
        }

        T0 await_resume()
        {
            winrt::check_hresult(m_error);
            m_state.reset();
            return m_async.GetResults();
        }
    };

    template <typename T0, typename T1>
    struct async_operation_progress_awaiter
    {
        winrt::Windows::Foundation::IAsyncOperationWithProgress<T0, T1> const& m_async;
        std::shared_ptr<winrt::impl::async_coroutine_state> m_state;
        std::int32_t m_error = winrt::hr_ok;

        explicit async_operation_progress_awaiter(winrt::Windows::Foundation::IAsyncOperationWithProgress<T0, T1> const& async) noexcept : m_async(async) {}

        bool await_ready() const noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<void> handle)
        {
            auto state = std::make_shared<winrt::impl::async_coroutine_state>(&m_error);
            m_state = state;
            state->prepare(handle);
            winrt::Windows::Foundation::AsyncOperationWithProgressCompletedHandler<T0, T1> handler
            {
                [state, async = m_async.get_abi()](
                    winrt::Windows::Foundation::IAsyncOperationWithProgress<T0, T1> const&,
                    winrt::Windows::Foundation::AsyncStatus status) noexcept
                {
                    state->complete_status(static_cast<std::int32_t>(status), async);
                }
            };
            m_async.put_Completed(handler);
            return state->suspend();
        }

        T0 await_resume()
        {
            winrt::check_hresult(m_error);
            m_state.reset();
            return m_async.GetResults();
        }
    };
)cppwinc", async_helpers);
}

void write_delegate(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const& metadata, std::string& winrt_specs, std::string& bodies)
{
    if (type.invoke < 0)
    {
        return;
    }
    auto const& method = type.methods[type.invoke];
    std::string cpp_name = type_cpp_name(type);
    bool generic = !type.generic_params.empty();
    std::string tpl = tpl_prefix(type, metadata);
    std::string targs = tpl_args(type);
    std::string qual = cpp_name + (generic ? "<" + targs + ">" : "");

    if (generic)
    {
        w.write("export %struct %;\n", tpl, type.name);
        winrt_specs += tpl_prefix(type, metadata) + "inline constexpr std::size_t interface_method_count<" + cpp_name + "<" + targs + ">> = 1;\n";
        winrt_specs += tpl_prefix(type, metadata) + "inline constexpr std::uint8_t type_kind<" + cpp_name + "<" + targs + ">> = 2;\n";
    }
    else
    {
        w.write("export struct %;\n", type.name);
        winrt_specs += "template <> inline constexpr winrt_guid iid_of<" + cpp_name + "> = " + guid_literal(type) + ";\n";
        winrt_specs += "template <> inline constexpr std::size_t interface_method_count<" + cpp_name + "> = 1;\n";
        winrt_specs += "template <> inline constexpr std::uint8_t type_kind<" + cpp_name + "> = 2;\n";
    }

    w.write("export %struct %\n{\n", tpl, type.name);
    w.write(R"cppwinc(    void* m_ptr = nullptr;

%() noexcept = default;
%(void* ptr) noexcept : m_ptr(ptr) {}
%(% const& other) : m_ptr(other.m_ptr) { if (m_ptr) IUnknown::addref_raw(m_ptr); }
%(%&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }
%& operator=(% const& other) { if (this != &other) { % copy(other); std::swap(m_ptr, copy.m_ptr); } return *this; }
%& operator=(%&& other) noexcept { if (this != &other) { m_ptr = other.m_ptr; other.m_ptr = nullptr; } return *this; }
~%() { if (m_ptr) IUnknown::release_raw(m_ptr); }
void* get_abi() const noexcept { return m_ptr; }
void** put() noexcept { if (m_ptr) IUnknown::release_raw(m_ptr); m_ptr = nullptr; return &m_ptr; }
static % from_abi(void* ptr) noexcept { return %(ptr); }
explicit operator bool() const noexcept { return m_ptr != nullptr; }
friend auto operator<=>(% const&, % const&) = default;
bool operator==(std::nullptr_t) const noexcept { return m_ptr == nullptr; }
)cppwinc",
        type.name,
        type.name, type.name,
        type.name, type.name, type.name,
        type.name, type.name, type.name,
        type.name, type.name, type.name,
        type.name, type.name,
        type.name, type.name, type.name, type.name);

    // callable construction: a lambda becomes a COM delegate via a nested
    // impl deriving winrt::impl::com_handler_base (IUnknown + Invoke).
    // The impl stores an ABI-signature callable; the projection
    // conversions happen in make_impl's lambda, which is a template and
    // therefore instantiated only in the consumer's translation unit,
    // where every projected type is complete.
    std::string impl_name = type.name + "_impl";
    std::string abi_fn_type = "void(";
    std::string abi_params = "void* self";
    std::string abi_call_params;
    std::string abi_arg_names;
    std::string conv_args;
    for (std::size_t p = 0; p < method.params.size(); ++p)
    {
        auto const& pt = method.params[p].value;
        std::string a = "a" + std::to_string(p);
        if (p)
        {
            abi_fn_type += ", ";
            abi_call_params += ", ";
            abi_arg_names += ", ";
            conv_args += ", ";
        }
        abi_params += ", ";
        if (pt.kind >= winmd::et_boolean && pt.kind <= winmd::et_r8 && pt.kind != winmd::et_string)
        {
            std::string t = primitive_cpp(pt.kind);
            abi_fn_type += t;
            abi_params += t + " " + a;
            abi_call_params += t + " " + a;
            abi_arg_names += a;
            conv_args += a;
        }
        else if (pt.kind == winmd::et_valuetype && metadata.find(pt) && metadata.find(pt)->is_enum())
        {
            abi_fn_type += "std::int32_t";
            abi_params += "std::int32_t " + a;
            abi_call_params += "std::int32_t " + a;
            abi_arg_names += a;
            conv_args += "static_cast<" + mapper.map(pt) + ">(" + a + ")";
        }
        else if (pt.kind == winmd::et_szarray)
        {
            abi_fn_type += "std::uint32_t, void*";
            abi_params += "std::uint32_t " + a + "_size, void* " + a + "_data";
            abi_call_params += "std::uint32_t " + a + "_size, void* " + a + "_data";
            abi_arg_names += a + "_size, " + a + "_data";
            conv_args += "winrt::span<" + mapper.map(pt.children.at(0)) + ">{ " + a + "_data, " + a + "_size }";
        }
        else if (pt.kind == winmd::et_string)
        {
            abi_fn_type += "void*";
            abi_params += "void* " + a;
            abi_call_params += "void* " + a;
            abi_arg_names += a;
            conv_args += "*reinterpret_cast<winrt::hstring const*>(&" + a + ")";
        }
        else if (pt.kind == winmd::et_valuetype)
        {
            if (mapper.map(pt) == "winrt::IInspectable")
            {
                // IInspectable params (the `sender`) arrive as a raw void*
                // slot carrying an unowned ref. A by-value wrapper parameter
                // would AddRef in its copy-ctor and Release in the invoke's
                // parameter dtor, stealing one OS ref per event raise.
                // Alias the slot instead: zero ctor/dtor activity, OS refs
                // untouched. The lambda sees a wrapper whose m_ptr is the
                // raw pointer - identical layout, no ownership.
                abi_fn_type += "void*";
                abi_params += "void* " + a;
                abi_call_params += "void* " + a;
                abi_arg_names += a;
                conv_args += "*reinterpret_cast<winrt::IInspectable const*>(&" + a + ")";
            }
            else
            {
                // value types travel by value in the ABI exactly as the OS
                // expects
                std::string t = mapper.map(pt);
                abi_fn_type += t;
                abi_params += t + " " + a;
                abi_call_params += t + " " + a;
                abi_arg_names += a;
                conv_args += a;
            }
        }
        else if (pt.kind == winmd::et_var || pt.kind == winmd::et_mvar)
        {
            // Generic params (TypedEventHandler<T0,T1> etc.) have unknown
            // ABI shapes until instantiation: arg_in_t<T> is a projected
            // struct by value for runtime classes, or void* for hstring /
            // com_ptr. Cast the ADDRESS of the slot to the exact projected
            // type the user lambda expects - every projected wrapper leads
            // with void* m_ptr, so the reinterpret is layout-identical for
            // both the struct and the bare-pointer shape.
            std::string t = "winrt::impl::arg_in_t<" + mapper.map(pt) + ">";
            abi_fn_type += t;
            abi_params += t + " " + a;
            abi_call_params += t + " " + a;
            abi_arg_names += a;
            conv_args += "*reinterpret_cast<" + mapper.map(pt) + " const*>(static_cast<void const*>(&" + a + "))";
        }
        else if (pt.kind == winmd::et_genericinst)
        {
            // Instantiated generic interfaces (IAsyncOperation<T0>,
            // TypedEventHandler<T0,T1>, ...): the ABI slot is void* and the
            // projected wrapper (void* m_ptr first member) aliases it, so
            // reinterpret to the exact projected instantiation the consumer
            // lambda expects - never to IInspectable, which would not convert
            // back at the call site (C2664).
            abi_fn_type += "void*";
            abi_params += "void* " + a;
            abi_call_params += "void* " + a;
            abi_arg_names += a;
            // Alias the SLOT, not the pointee (see et_class above): the
            // projected wrapper's m_ptr must become the raw object pointer.
            conv_args += "*reinterpret_cast<" + mapper.map(pt) + " const*>(&" + a + ")";
        }
        else if (pt.kind == winmd::et_class)
        {
            auto const* found = metadata.find(pt);
            if (found && is_foundation_type(found->ns, found->name) && found->name == "IInspectable")
            {
                // IInspectable params (the `sender`) arrive as a raw void*
                // slot carrying an unowned ref. Same treatment as the
                // et_valuetype IInspectable case above: alias the slot — a
                // by-value wrapper param would take ownership of the raw
                // pointer and Release it at invoke return (−1 OS ref per
                // raise).
                abi_fn_type += "void*";
                abi_params += "void* " + a;
                abi_call_params += "void* " + a;
                abi_arg_names += a;
                conv_args += "*reinterpret_cast<winrt::IInspectable const*>(&" + a + ")";
            }
            else
            {
                abi_fn_type += "void*";
                abi_params += "void* " + a;
                abi_call_params += "void* " + a;
                abi_arg_names += a;
                // Alias the SLOT, not the pointee: the projected wrapper
                // leads with void* m_ptr, so reinterpreting the parameter's
                // address yields m_ptr = the raw object pointer. Reinterpreting
                // the VALUE instead would read the object's vtable as m_ptr.
                conv_args += "*reinterpret_cast<" + mapper.map(pt) + " const*>(&" + a + ")";
            }
        }
        else
        {
            abi_fn_type += "void*";
            abi_params += "void* " + a;
            abi_call_params += "void* " + a;
            abi_arg_names += a;
            conv_args += "*reinterpret_cast<winrt::IInspectable const*>(&" + a + ")";
        }
    }
    abi_fn_type += ")";
    w.write("    template <typename L>\n");
    w.write("    %(L&& lambda) : m_ptr(make_impl(std::forward<L>(lambda))) {}\n\n", type.name);
    w.write("    template <typename L>\n");
    w.write("    static void* make_impl(L&& lambda)\n    {\n");
    w.write("        auto* impl = new %{ [c = std::forward<L>(lambda)](% ) mutable { c(%); } };\n", impl_name, abi_call_params, conv_args);
    w.write("        return impl;\n    }\n\n");
    w.write("    struct % : winrt::impl::com_handler_base<%>\n    {\n", impl_name, impl_name);
    w.write("        std::function<%> m_callable;\n", abi_fn_type);
    w.write("        explicit %(std::function<%> const& callable) : m_callable(callable) {}\n", impl_name, abi_fn_type);
    w.write("        static std::int32_t __stdcall invoke(% ) noexcept\n        {\n", abi_params);
    w.write("            auto* me = static_cast<%*>(self);\n", impl_name);
    w.write("            me->m_callable(%);\n            return 0;\n        }\n", abi_arg_names);
    if (generic)
    {
        std::string args;
        for (std::size_t p = 0; p < type.generic_params.size(); ++p)
        {
            args += (p ? ", " : "") + std::string("winrt::generic_arg_of<T") + std::to_string(p) + ">";
        }
        w.write("        static winrt::winrt_guid const& iid() noexcept\n        {\n");
        w.write("            static winrt::winrt_guid const value = winrt::impl::make_generic_iid(% , %);\n", guid_literal(type), args);
        w.write("            return value;\n        }\n");
    }
    else
    {
        w.write("        static winrt::winrt_guid const& iid() noexcept\n        {\n");
        w.write("            static winrt::winrt_guid const value = %;\n            return value;\n        }\n", guid_literal(type));
    }
    w.write("    };\n\n");

    w.write("\n    %;\n", method_sig(method, mapper, true));
    bodies += out_of_line(type, metadata, method_sig(method, mapper, true, qual),
        abi_call_body(method, "m_ptr", 6, mapper, metadata, "    "));
    w.write("};\n\n");
}

void write_class(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const& metadata, std::string& winrt_specs, std::string& bodies)
{
    std::string cpp_name = type_cpp_name(type);
    bool generic = !type.generic_params.empty();
    std::string tpl = tpl_prefix(type, metadata);
    std::string targs = tpl_args(type);
    std::string qual = cpp_name + (generic ? "<" + targs + ">" : "");
    winrt_specs += "template <> inline constexpr std::uint8_t type_kind<" + cpp_name + "> = 1;\n";
    winrt_specs += "template <> inline constexpr std::string_view type_name<" + cpp_name + "> = \"" + type.full_name() + "\";\n";
    if (type.default_interface)
    {
        winrt_specs += "template <> inline constexpr winrt_guid iid_of<" + cpp_name + "> = " + guid_literal(*type.default_interface) + ";\n";
    }
    w.write("export %struct %\n{\n", tpl, type.name);

    // activation member data
    w.write("    static constexpr std::wstring_view class_name() noexcept { return L\"%\"; }\n", type.full_name());
    if (type.default_interface)
        w.write("    static constexpr winrt::winrt_guid abi_iid = %;\n", guid_literal(*type.default_interface));
    w.write("    winrt::com_ptr<void> m_unknown;\n");
    w.write("    mutable winrt::impl::iface_cache m_cache;\n\n");

    // dispatcher queues take a simple callback delegate; expose its
    // projection type so winrt::resume_foreground can construct it
    std::string full = type.full_name();
    if (full == "Windows.System.DispatcherQueue" || full == "Microsoft.UI.Dispatching.DispatcherQueue")
    {
        w.write("    using handler_type = DispatcherQueueHandler;\n\n");
    }

    // Wrap-tag ctor: from_abi must NEVER activate a new runtime object — it
    // wraps an existing pointer. The composable default ctor below activates
    // via the factory; the two must be distinct or every from_abi call
    // (Application::get_Current, base-class conversions, ...) would create a
    // second Application/Window and crash XAML.
    w.write(R"cppwinc(    struct wrap_tag_t { explicit wrap_tag_t() = default; };
    %(wrap_tag_t) noexcept {}

    static % from_abi(void* ptr) noexcept { % result{wrap_tag_t{}}; result.m_unknown = winrt::com_ptr<void>::attach(ptr); return result; }

    // raw ABI handle: required by impl::arg_in/bind_in so runtime-class
    // wrappers pass their interface POINTER (not the wrapper struct) into
    // generic-parameter ABI slots (IVector<ResourceDictionary>::Append,
    // Style.Setters, event args, ...), and by collection_element for GetAt.
    void* get_abi() const noexcept { return m_unknown.get(); }

template <typename I>
void* iface() const
{
    // Query with the REQUESTED interface's IID: iid_of<I> is specialized for
    // every projected interface (its own IID) and every projected class (its
    // default interface's IID), so both iface<IWindow2>() and iface<Self>()
    // resolve to the correct vtable. Querying the class default IID
    // unconditionally (iid_of_abi<decltype(*this)>) returns a pointer whose
    // vtable is too short for non-default interfaces -> access violation.
    void* result = m_cache.get(winrt::iid_of<std::remove_cvref_t<I>>, m_unknown.get());
    if (!result)
    {
        throw winrt::hresult_error(winrt::hr_no_interface);
    }
    return result;
}

// Borrowed-pointer conversions MUST AddRef: the resulting wrapper OWNS its
// reference (its dtor releases). Without the AddRef, every implicit conversion
// to IInspectable or a base projected type (Append(child), Content(...),
// Source(...), ...) steals one of this wrapper's references -> over-release ->
// heap corruption (use-after-free on the next GC/alloc cycle).
void* addref_borrowed() const noexcept
{
    if (m_unknown.get())
    {
        auto* vt = *reinterpret_cast<void* const**>(m_unknown.get());
        reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(vt[1])(m_unknown.get());
    }
    return m_unknown.get();
}

// Implicit conversion to IInspectable for property setters
operator winrt::IInspectable() const noexcept { return winrt::IInspectable(addref_borrowed()); }

// Cross-class conversion: projected classes are flat wrappers (no C++
// inheritance), but the runtime class hierarchy is real - BitmapImage IS an
// ImageSource, Button IS a ContentControl. A parameter of a base projected
// type (Image::Source, Panel::Children.Append, ...) must accept any derived
// projected wrapper. AddRef the shared COM pointer (the target wrapper owns
// it; from_abi attaches); the target's iface() QIs through IUnknown, so
// the query is correct regardless of which wrapper initiated it. Guarded by
// a requires clause so it never competes with IInspectable's own copy/move.
template <typename U> requires requires(U const& u) { u.m_unknown; }
operator U() const noexcept { return U::from_abi(addref_borrowed()); }
)cppwinc",
        type.name, type.name, type.name);
    w.write("    static % wrap_borrowed(void* ptr) noexcept\n    {\n        if (ptr)\n        {\n            auto* vt = *reinterpret_cast<void* const**>(ptr);\n            reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(vt[1])(ptr);\n        }\n        return from_abi(ptr);\n    }\n\n", type.name);

    // default constructor (activation)
    // Abstract-like = metadata abstract flag OR a composable factory with no
    // creation method (empty IUIElementFactory-style factory: XAML's abstract
    // base classes declare composable factories that carry zero methods, so
    // nothing can legitimately create them). Both get a null-wrapper default
    // ctor: m_unknown stays empty - no factory lookup, no empty-vtable slot-6
    // call, no AV. Construction-only uses (XAML base stubs via 'using T::T;')
    // are safe; real *use* of such a null wrapper fails loudly instead.
    bool const abstract_like = (type.flags & winmd::attr_abstract) != 0 ||
        (type.composable && !type.default_activatable && !type.factory_can_create());
    if (abstract_like)
    {
        // Abstract runtime classes (UIElement, Geometry, Style, Brush, ...)
        // have no activation path. The default ctor constructs a NULL wrapper
        // (public so stubs using 'using T::T;' still compile): m_unknown stays
        // empty, no factory activation, no crash. Any real *use* of the null
        // wrapper throws/AVs loudly - but the emitter comments out non-visual
        // uses of such values, so construction-only is the expected pattern.
        w.write("\n    %() noexcept = default; // abstract runtime class: null wrapper, no factory activation\n", type.name);
    }
    else if (type.default_activatable)
    {
        w.write("\n    %()\n        : m_unknown(winrt::impl::create_instance(class_name()))\n    {\n    }\n", type.name);
    }
    else if (type.composable)
    {
        // Composable runtime classes (Window, Application, ContentControl, ...)
        // carry no default ActivatableAttribute: RoActivateInstance fails with
        // REGDB_E_CLASSNOTAVAILABLE for them. The OS composition contract
        // requires creating them through their factory: RoGetActivationFactory
        // -> IFactory::CreateInstance(outer, &inner). The factory pointer is
        // the *inner*, so the wrapper must QI the created instance - never the
        // factory - for IUnknown/IInspectable. IFactory vtable: IInspectable
        // occupies slots 0..5; CreateInstance(outer, inner) is slot 6.
        if (type.composable_factory)
        {
            // Parameterized factory support: some factories take extra
            // leading in-params on their creation method (e.g.
            // IFontFamilyFactory::CreateInstanceWithName(familyName, base,
            // inner*, out*)). Calling slot 6 with the plain IFactory shape
            // on those feeds garbage (the out-inner pointer is read as the
            // first extra param) -> E_INVALIDARG / AV inside XAML. Locate
            // the real creation method, its vtable slot, and emit the exact
            // ABI signature with neutral defaults for the extras.
            std::string create_fn_type = "std::int32_t(__stdcall*)(void*, void*, void**, void**)";
            std::string extra_decls;
            std::string extra_args;
            std::string ctor_sig;
            std::string create_slot_s = "6";
            winmd::method_info const* create_method = nullptr;
            bool has_plain_create = false;
            {
                auto const& fmethods = type.composable_factory->methods;
                // First pass: does a PLAIN CreateInstance(base, inner) exist?
                // If yes, the class is default-activatable through the factory
                // and the parameterless ctor must use THAT method (never a
                // parameterized one - FontFamily has only CreateInstanceWithName,
                // ComboBox has both and must keep the plain one).
                for (std::size_t mi = 0; mi < fmethods.size(); ++mi)
                {
                    auto const& m = fmethods[mi];
                    auto const& ps = m.params;
                    if (ps.size() != 2) continue;
                    if ((ps[ps.size() - 1].flags & winmd::param_out) == 0) continue;
                    if ((ps[ps.size() - 2].flags & winmd::param_out) != 0) continue;
                    if (m.ret.kind == winmd::et_void) continue;
                    has_plain_create = true;
                    create_method = &m;
                    create_slot_s = std::to_string(6 + mi);
                    break;
                }
                if (!has_plain_create)
                {
                    // Second pass: fall back to the first parameterized
                    // creation method ([...extras, in base, out inner],
                    // ret = the created class).
                    for (std::size_t mi = 0; mi < fmethods.size(); ++mi)
                    {
                        auto const& m = fmethods[mi];
                        auto const& ps = m.params;
                        if (ps.size() < 2) continue;
                        if ((ps[ps.size() - 1].flags & winmd::param_out) == 0) continue;
                        if ((ps[ps.size() - 2].flags & winmd::param_out) != 0) continue;
                        if (m.ret.kind == winmd::et_void) continue;
                        create_method = &m;
                        create_slot_s = std::to_string(6 + mi);
                        break;
                    }
                }
            }
            bool parameterized_ctor = (create_method && create_method->params.size() > 2 && !has_plain_create);
            if (parameterized_ctor)
            {
                create_fn_type = "std::int32_t(__stdcall*)(void*";
                for (std::size_t ei = 0; ei + 2 < create_method->params.size(); ++ei)
                {
                    auto const& p = create_method->params[ei];
                    std::string pn = "p" + std::to_string(ei);
                    // every extra arg carries its own leading comma: the call
                    // template is `create(f%, nullptr, ...)`
                    std::string sep = ", ";
                    if (p.value.kind == winmd::et_string)
                    {
                        create_fn_type += ", void*";
                        // real ctor parameter - a default-constructed hstring
                        // has a NULL ABI value and factories reject it with
                        // E_INVALIDARG (ARG_NOTNULL in the OS factory code)
                        ctor_sig += (ctor_sig.empty() ? "" : ", ") + std::string("winrt::hstring const& ") + pn;
                        extra_args += sep + pn + ".get_abi()";
                    }
                    else if (p.value.kind == winmd::et_szarray)
                    {
                        create_fn_type += ", std::uint32_t, void*";
                        extra_decls += "        std::uint32_t " + pn + "_len{0};\n        void* " + pn + "_data{nullptr};\n";
                        extra_args += sep + pn + "_len, " + pn + "_data";
                    }
                    else if (p.value.kind >= winmd::et_boolean && p.value.kind <= winmd::et_r8)
                    {
                        create_fn_type += ", " + primitive_cpp(p.value.kind);
                        ctor_sig += (ctor_sig.empty() ? "" : ", ") + primitive_cpp(p.value.kind) + " " + pn;
                        extra_args += sep + pn;
                    }
                    else if (p.value.kind == winmd::et_valuetype)
                    {
                        auto* pt = metadata.find(p.value);
                        if (pt && pt->is_enum())
                        {
                            create_fn_type += ", std::int32_t";
                            ctor_sig += (ctor_sig.empty() ? "" : ", ") + std::string("std::int32_t ") + pn;
                            extra_args += sep + pn;
                        }
                        else
                        {
                            // struct extra: hidden-ref slot; pass a null-filled
                            // buffer (no known factories take struct extras)
                            create_fn_type += ", void*";
                            extra_decls += "        void* " + pn + "{nullptr};\n";
                            extra_args += sep + "reinterpret_cast<void*>(&" + pn + ")";
                        }
                    }
                    else
                    {
                        // interface/class/delegate extra: null ABI pointer
                        create_fn_type += ", void*";
                        extra_decls += "        void* " + pn + "{nullptr};\n";
                        extra_args += sep + pn;
                    }
                }
                create_fn_type += ", void*, void**, void**)";
            }
            // Resolve the factory interface explicitly with a QI: robust even
            // if the activation factory's default interface is not IFactory.
            // The IID is a GUID literal: the factory interface may not be
            // projected at all, and an iid_of<Factory> specialization used
            // inside the class body instantiates before its explicit
            // specialization appears later in the module (C2908).
            w.write(
                "\n    %(%)\n    {\n"
                "        winrt::boot_log(\"win:ctor-begin\");\n"
                "        winrt::boot_log(\"win:ctor-class:%\");\n"
                "        // cppwinrt-style app-lifetime factory cache: the activation\n"
                "        // factory is cached in a static com_ptr and NEVER released.\n"
                "        // Releasing it per-construction races with OS holders and can\n"
                "        // free the factory while ctor locals still reference it.\n"
                "        static winrt::com_ptr<void> const s_factory = winrt::com_ptr<void>::attach(winrt::impl::get_factory(class_name()));\n"
                "        winrt::boot_log(\"win:factory\");\n"
                "        void* f = nullptr;\n"
                "        winrt::check_hresult(winrt::impl::query_interface(s_factory.get(), %, &f));\n"
                "        winrt::boot_log(\"win:qi-factory\");\n"
                "        auto* fvt = *reinterpret_cast<void* const**>(f);\n"
                "        // Factory creation slot: IInspectable 0..5, then the\n"
                "        // creation method (CreateInstance or a parameterized\n"
                "        // CreateInstanceXxx taking extra leading in-params).\n"
                "        auto create = reinterpret_cast<%>(fvt[%]);\n"
                "%"
                "        void* inner = nullptr;\n"
                "        void* instance = nullptr;\n"
                "        std::int32_t create_hr = create(f%, nullptr, &inner, &instance);\n"
                "        if (create_hr < 0)\n"
                "        {\n"
                "            // cppwinrt emits CreateInstance(null, null, &instance) for\n"
                "            // classes with no composable bases; some factories reject\n"
                "            // the out-inner shape (E_INVALIDARG). Retry with the\n"
                "            // nulls-inner shape before failing.\n"
                "            void* instance2 = nullptr;\n"
                "            std::int32_t retry_hr = create(f%, nullptr, nullptr, &instance2);\n"
                "            if (retry_hr >= 0)\n"
                "            {\n"
                "                instance = instance2;\n"
                "                create_hr = retry_hr;\n"
                "            }\n"
                "            else\n"
                "            {\n"
                "                winrt::boot_logf(\"win:create-hr 0x%%08lX\", reinterpret_cast<void*>(static_cast<unsigned long>(create_hr)));\n"
                "                winrt::boot_logf(\"win:create-retry-hr 0x%%08lX\", reinterpret_cast<void*>(static_cast<unsigned long>(retry_hr)));\n"
                "            }\n"
                "        }\n"
                "        winrt::check_hresult(create_hr);\n"
                "        winrt::boot_log(\"win:create\");\n"
                "        auto* frel = *reinterpret_cast<void* const**>(f);\n"
                "        reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(frel[2])(f);\n"
                "        void* unknown = nullptr;\n"
                "        winrt::check_hresult(winrt::impl::query_interface(instance, winrt::iid_of<winrt::IInspectable>, &unknown));\n"
                "        winrt::boot_log(\"win:qi-instance\");\n"
                "        // create() returned +1 refs on instance and inner. We keep the\n"
                "        // QI'd ref on instance, so drop the raw instance ref. With a\n"
                "        // null outer nothing else holds inner: the instance tear-off\n"
                "        // delegates into it for its whole lifetime, so we must NOT\n"
                "        // release it (releasing frees the implementation under the\n"
                "        // instance - the exact use-after-free this ctor used to hit).\n"
                "        auto* irel = *reinterpret_cast<void* const**>(instance);\n"
                "        reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(irel[2])(instance);\n"
                "        winrt::boot_log(\"win:rels-done\");\n"
                "        m_unknown = winrt::com_ptr<void>::attach(unknown);\n"
                "        winrt::boot_log(\"win:ctor-end\");\n"
                "    }\n", type.name, ctor_sig, type.name, guid_literal(*type.composable_factory), create_fn_type, create_slot_s, extra_decls, extra_args, extra_args);
        }
        else
        {
            // Factory interface unknown/unprojected: the activation factory's
            // default interface is the class factory in practice.
            w.write(
                "\n    %()\n    {\n"
                "        winrt::boot_log(\"win:ctor-class:%\");\n"
                "        auto factory = winrt::com_ptr<void>::attach(winrt::impl::get_factory(class_name()));\n"
                "        auto* fvt = *reinterpret_cast<void* const**>(factory.get());\n"
                "        auto create = reinterpret_cast<std::int32_t(__stdcall*)(void*, void*, void**, void**)>(fvt[6]);\n"
                "        void* inner = nullptr;\n"
                "        void* instance = nullptr;\n"
                "        winrt::check_hresult(create(factory.get(), nullptr, &inner, &instance));\n"
                "        void* unknown = nullptr;\n"
                "        winrt::check_hresult(winrt::impl::query_interface(instance, winrt::iid_of<winrt::IInspectable>, &unknown));\n"
                "        auto* irel = *reinterpret_cast<void* const**>(instance);\n"
                "        reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(irel[2])(instance);\n"
                "        // Keep inner alive: with a null outer the instance tear-off\n"
                "        // delegates into it for its whole lifetime (see branch above).\n"
                "        m_unknown = winrt::com_ptr<void>::attach(unknown);\n"
                "    }\n", type.name, type.name);
        }
    }
    else
    {
        w.write("\n    %() noexcept = default;\n", type.name);
    }

    // statics-accessor collision keys: when a statics interface exposes an accessor with
    // the same name and parameter types as an instance accessor, the static wins on the
    // class (mirrors the real projection, where the class-declared static hides the
    // inherited interface member) — otherwise C2686
    auto method_key = [&mapper](winmd::method_info const& method)
    {
        std::string key = method.name;
        for (auto const& p : method.params)
        {
            key += "|";
            key += mapper.map(p.value);
        }
        return key;
    };
    std::set<std::string> static_keys;
    std::set<std::string> emitted_keys;
    for (auto const* statics : type.statics)
    {
        if (!statics)
        {
            continue;
        }
        for (auto const* method : flattened_methods(*statics, metadata))
        {
            static_keys.insert(method_key(*method));
        }
    }

    // instance methods (own interfaces)
    for (auto const* iface : type.interfaces)
    {
        if (!iface || !iface->is_interface())
        {
            continue;
        }
        auto methods = flattened_methods(*iface, metadata);
        for (std::size_t i = 0; i < methods.size(); ++i)
        {
            auto const& method = *methods[i];
            if (method.is_static)
            {
                continue;
            }
            // a flattened base-interface method may already be emitted via an
            // earlier interface in the list — dedupe (C2535 otherwise)
            if (!emitted_keys.insert(method_key(method)).second)
            {
                continue;
            }
            if (static_keys.contains(method_key(method)))
            {
                continue;
            }

    w.write("\n    %;\n", method_sig(method, mapper, true));
            writer b;
            b.write("        auto* p = iface<%>();\n", type_cpp_name(*iface));
            b.write("        if (!p) throw winrt::hresult_error(winrt::hr_no_interface);\n");
            b.write("%", abi_call_body(method, "p", 6 + i, mapper, metadata, "        "));
            bodies += out_of_line(type, metadata, method_sig(method, mapper, true, qual), b.str());
        }
    }

    // inherited instance methods (walk base chain, capped at 200 per type)
    {
        constexpr std::size_t MAX_INHERITED = 5000;
        winmd::type_info const* base = type.base;
        std::set<std::string, std::less<>> inherited_keys;
        std::size_t inherited_count = 0;
        while (base && inherited_count < MAX_INHERITED)
        {
            for (auto const* base_iface : base->interfaces)
            {
                if (!base_iface || !base_iface->is_interface())
                {
                    continue;
                }
                auto base_methods = flattened_methods(*base_iface, metadata);
                for (std::size_t i = 0; i < base_methods.size(); ++i)
                {
                    if (inherited_count >= MAX_INHERITED) break;
                    auto const& method = *base_methods[i];
                    if (method.is_static)
                    {
                        continue;
                    }
                    if (!emitted_keys.insert(method_key(method)).second)
                    {
                        continue;
                    }
                    if (!inherited_keys.insert(method_key(method)).second)
                    {
                        continue;
                    }
                    ++inherited_count;

                    w.write("\n    %;\n", method_sig(method, mapper, true));
                    writer b;
                    b.write("        auto* p = iface<%>();\n", type_cpp_name(*base_iface));
                    b.write("        if (!p) throw winrt::hresult_error(winrt::hr_no_interface);\n");
                    b.write("%", abi_call_body(method, "p", 6 + i, mapper, metadata, "        "));
                    bodies += out_of_line(type, metadata, method_sig(method, mapper, true, qual), b.str());
                }
            }
            base = base->base;
        }
    }

    // statics
    for (auto const* statics : type.statics)
    {
        if (!statics)
        {
            continue;
        }
        auto methods = flattened_methods(*statics, metadata);
        for (std::size_t i = 0; i < methods.size(); ++i)
        {
            auto const& method = *methods[i];
            // multiple statics interfaces may expose the same member
            // (e.g. WebView2 "Core" metadata) — dedupe (C2535 otherwise)
            if (!emitted_keys.insert(method_key(method)).second)
            {
                continue;
            }
            w.write("\n    static %;\n", method_sig(method, mapper, false));
            writer b;
            // the metadata may name a parameter "factory" — avoid
            // colliding with the activation-factory local (C2082)
            std::string factory_var = "factory";
            for (auto const& p : method.params)
            {
                if (p.name == factory_var)
                {
                    factory_var += "_";
                }
            }
            b.write("        auto % = winrt::get_activation_factory<%>();\n", factory_var, cpp_name);
            b.write("        void* abi = nullptr;\n");
            b.write("        winrt::check_hresult(winrt::impl::query_interface(%.get(), winrt::iid_of<%>, &abi));\n", factory_var, type_cpp_name(*statics));
            b.write("        if (!abi) throw winrt::hresult_error(winrt::hr_no_interface);\n");
            b.write("%", abi_call_body(method, "abi", 6 + i, mapper, metadata, "        "));
            bodies += out_of_line(type, metadata, method_sig(method, mapper, false, qual), b.str());
        }
    }

    // parameterized constructors — each activation-factory interface method
    // becomes a class constructor (e.g. IUriRuntimeClassFactory::CreateUri(hstring)
    // projects as Uri(hstring))
    {
        std::set<std::string> ctor_keys;
        for (auto const* factory_iface : type.activatable)
        {
            if (!factory_iface || !factory_iface->is_interface())
            {
                continue;
            }
            auto methods = flattened_methods(*factory_iface, metadata);
            for (std::size_t i = 0; i < methods.size(); ++i)
            {
                auto const& method = *methods[i];
                if (method.is_static || method.params.empty())
                {
                    // a parameterless factory method would collide with the
                    // default constructor (C2535)
                    continue;
                }
                std::string params;
                for (std::size_t p = 0; p < method.params.size(); ++p)
                {
                    if (p) params += ", ";
                    std::string name = method.params[p].name.empty() ? "arg" + std::to_string(p) : method.params[p].name;
                    params += param_decl(method.params[p].value, name, mapper, (method.params[p].flags & winmd::param_out) != 0);
                }
                // two factory interfaces may expose equivalent creation
                // methods — dedupe on the constructor signature (C2535)
                std::string key;
                for (auto const& p : method.params)
                {
                    key += "|";
                    key += mapper.map(p.value);
                }
                if (!ctor_keys.insert(key).second)
                {
                    continue;
                }
                w.write("\n    %(%);\n",
                    method.params.size() == 1 ? "explicit " + type.name : type.name, params);
                writer b;
                // the metadata may name a parameter "factory" — avoid
                // colliding with the activation-factory local (C2082)
                std::string factory_var = "factory";
                for (auto const& p : method.params)
                {
                    if (p.name == factory_var)
                    {
                        factory_var += "_";
                    }
                }
                b.write("        auto % = winrt::get_activation_factory<%>();\n", factory_var, cpp_name);
                b.write("        void* abi = nullptr;\n");
                b.write("        winrt::check_hresult(winrt::impl::query_interface(%.get(), winrt::iid_of<%>, &abi));\n", factory_var, type_cpp_name(*factory_iface));
                b.write("        if (!abi) throw winrt::hresult_error(winrt::hr_no_interface);\n");
                std::string body = abi_call_body(method, "abi", 6 + i, mapper, metadata, "        ");
                // a constructor cannot return — re-target the return-value
                // conversion to the object being constructed
                std::string marker = "return ";
                auto pos = body.rfind(marker);
                if (pos != std::string::npos)
                {
                    body.replace(pos, marker.size(), "*this = ");
                }
                b.write("%", body);
                bodies += out_of_line(type, metadata, cpp_name + "::" + type.name + "(" + params + ")", b.str());
            }
        }
    }

    // property wrappers: Content() getter/setter, Content(value) setter
    // Collect all emitted get_/put_ and add_/remove_ pairs to generate
    // clean property syntax: obj.Property() for getter, obj.Property(val) for setter
    {
        // Collect all method keys from statics/factory interfaces so we
        // can skip them in wrapper generation.  This prevents generating
        // broken instance wrappers for static properties (e.g. Colors.AliceBlue
        // which lives on IColorsStatics, not as an instance method).
        std::set<std::string> statics_keys;
        for (auto const* statics : type.statics)
        {
            if (!statics) continue;
            for (auto const* m : flattened_methods(*statics, metadata))
                statics_keys.insert(method_key(*m));
        }
        std::set<std::string> wrapper_emitted;
        auto collect_wrappers = [&](auto const& methods_list) {
            for (std::size_t i = 0; i < methods_list.size(); ++i)
            {
                auto const& m = *methods_list[i];
                if (m.is_static) continue;
                // Skip methods that also appear in a statics interface —
                // they are static properties, not instance members.
                if (statics_keys.contains(method_key(m))) continue;
                std::string name = m.name;
                // get_X -> property X (getter)
                // Emit declaration only; body is out-of-line to avoid
                // incomplete-type errors when the return type is defined
                // later in the same module.
                if (name.starts_with("get_") && name.size() > 4)
                {
                    std::string prop = name.substr(4);
                    if (wrapper_emitted.insert("get_" + prop).second && m.params.empty())
                    {
                        std::string ret = mapper.map(m.ret);
                        w.write("    % %() const;\n", ret, prop);
                        // Also mark the bare name as emitted so collection injection
                        // doesn't duplicate it (e.g. get_Size -> Size)
                        emitted_keys.insert(prop + "|");
                        bodies += out_of_line(type, metadata,
                            ret + " " + qual + "::" + prop + "() const",
                            "    return get_" + prop + "();\n");
                    }
                }
                // put_X -> property X (setter)
                if (name.starts_with("put_") && name.size() > 4)
                {
                    std::string prop = name.substr(4);
                    if (wrapper_emitted.insert("put_" + prop).second && m.params.size() == 1)
                    {
                        std::string param_type = mapper.map(m.params[0].value);
                        // For IInspectable params (Content, Tag, etc.), generate
                        // hstring-accepting wrapper that auto-boxes string values
                        if (param_type == "winrt::IInspectable")
                        {
                            w.write("    void %(const winrt::hstring& value) const { put_%(winrt::IInspectable(winrt::box_value(value))); }\n", prop, prop);
                            w.write("    void %(const winrt::IInspectable& value) const { put_%(value); }\n", prop, prop);
                        }
                        else
                            w.write("    void %(const %& value) const { put_%(value); }\n", prop, param_type, prop);
                    }
                }
                // add_X / remove_X -> event X(handler)
                if (name.starts_with("add_") && name.size() > 4)
                {
                    std::string evname = name.substr(4);
                    if (wrapper_emitted.insert("add_" + evname).second && m.params.size() == 1)
                    {
                        std::string param_type = mapper.map(m.params[0].value);
                        std::string ret_mapped = mapper.map(m.ret);
                        // Use the actual return type of add_X — some events return
                        // IInspectable instead of event_token (e.g. Application events)
                        w.write("    % %(const %& handler) const { return add_%(handler); }\n", ret_mapped, evname, param_type, evname);
                    }
                }
            }
        };
        for (auto const* iface : type.interfaces)
        {
            if (!iface || !iface->is_interface()) continue;
            // Skip statics/factory interfaces — their methods are not
            // instance members of the class and cannot have wrappers.
            std::string iname = iface->name;
            if (iname.find("Static") != std::string::npos ||
                iname.find("Factory") != std::string::npos) continue;
            collect_wrappers(flattened_methods(*iface, metadata));
        }
        {
            winmd::type_info const* b = type.base;
            while (b)
            {
                for (auto const* iface : b->interfaces)
                {
                    if (!iface || !iface->is_interface()) continue;
                    std::string bname = iface->name;
                    if (bname.find("Static") != std::string::npos ||
                        bname.find("Factory") != std::string::npos) continue;
                    collect_wrappers(flattened_methods(*iface, metadata));
                }
                b = b->base;
            }
        }
    }

    // Collection types: inject IVector<T> methods for XAML collection
    // classes that implement IVector<IInspectable> at ABI level.
    // Only for specific Xaml.Controls collections that need Append/GetAt.
    {
        static const std::set<std::string> known_xaml_collections = {
            "Microsoft.UI.Xaml.Controls.UIElementCollection",
            "Microsoft.UI.Xaml.Controls.ColumnDefinitionCollection",
            "Microsoft.UI.Xaml.Controls.RowDefinitionCollection",
            "Microsoft.UI.Xaml.Controls.ItemCollection",
            "Microsoft.UI.Xaml.Controls.MenuBarItemCollection",
            "Microsoft.UI.Xaml.Controls.NavigationViewItemCollection",
            "Microsoft.UI.Xaml.Controls.TabViewItemCollection",
            "Microsoft.UI.Xaml.Controls.Primitives.ColumnMajorDoubleUICollection",
            "Microsoft.UI.Xaml.Controls.Primitives.RowMajorDoubleUICollection",
            // media collections used by parse_svg_path (PathIcon.Data)
            "Microsoft.UI.Xaml.Media.PathFigureCollection",
            "Microsoft.UI.Xaml.Media.PathSegmentCollection",
        };
        std::string full_class = type.full_name();
        if (known_xaml_collections.contains(full_class))
        {
            bool has_append = emitted_keys.find("Append|") != emitted_keys.end();
            if (!has_append)
            {
                // The collection's closed IVector<T> IID must be computed from
                // the metadata (pattern GUID + OS pinterface argument
                // signature), not assumed IInspectable: XAML media collections
                // are IVector<T> for element classes (PathFigureCollection :
                // IVector<PathFigure>), and a wrong IID makes every QI fail
                // (E_NOINTERFACE -> hresult_error -> stowed exception
                // c000027b at the first Append / SVG path parse).
                std::string elem_cpp;
                std::string elem_sig_expr;
                std::string pattern_lit;
                bool const trace_coll = std::getenv("CPPWINC_TRACE") != nullptr;
                {
                    std::string generic_name;
                    std::vector<winmd::type> gargs;
                    bool got_sig = type.db && type.db->default_iface_sig(type.row, generic_name, gargs);
                    if (trace_coll)
                        std::fprintf(stderr, "TRACE: coll %s sig=%d name='%s' args=%zu\n",
                            full_class.c_str(), got_sig ? 1 : 0, generic_name.c_str(), gargs.size());
                    if (got_sig && !gargs.empty())
                    {
                        std::string ns, name;
                        bool resolved = metadata.resolve_type_name(gargs[0], ns, name);
                        if (trace_coll)
                            std::fprintf(stderr, "TRACE: elem resolved=%d ns='%s' name='%s'\n",
                                resolved ? 1 : 0, ns.c_str(), name.c_str());
                        if (!resolved)
                        {
                            // et_object (IInspectable) or unresolvable arg:
                            // IInspectable element - make_generic_iid emits
                            // the OS cinterface(IInspectable) spelling for it.
                            // The pattern (e.g. IObservableVector for
                            // ItemCollection) is kept from generic_name.
                            elem_cpp = "winrt::IInspectable";
                            elem_sig_expr = "winrt::generic_arg{ 0, \"\", winrt::iid_of<winrt::IInspectable>, \"\" }";
                        }
                        else
                        {
                            // IInspectable element: the OS pinterface grammar
                            // spells it cinterface(IInspectable) - route it
                            // through make_generic_iid's dedicated branch by
                            // passing kind 0 + the IInspectable IID (a
                            // rc(name;guid) encoding would compute a wrong
                            // closed IID and make every QI fail).
                            if (ns == "Windows.Foundation" && name == "IInspectable")
                            {
                                elem_cpp = "winrt::IInspectable";
                                elem_sig_expr = "winrt::generic_arg{ 0, \"\", winrt::iid_of<winrt::IInspectable>, \"\" }";
                            }
                            else
                            {
                                auto* etype = metadata.find(ns, name);
                                // Runtime classes carry no GuidAttribute in
                                // WinMD (only interfaces do); the rc(...)
                                // signature needs the class's DEFAULT
                                // INTERFACE GUID (the OS uses the default
                                // interface GUID in the class signature).
                                winmd::type_info const* eguid_src = nullptr;
                                if (etype)
                                {
                                    bool const et_is_class = (etype->kind == static_cast<winmd::type_kind>(0));
                                    if (et_is_class)
                                        eguid_src = etype->default_interface;
                                    else if (etype->has_guid)
                                        eguid_src = etype;
                                }
                                if (eguid_src && eguid_src->has_guid)
                                {
                                    bool const is_iface = (etype->kind == static_cast<winmd::type_kind>(1));
                                    bool const is_del = (etype->kind == static_cast<winmd::type_kind>(4));
                                    bool const is_class = (etype->kind == static_cast<winmd::type_kind>(0));
                                    elem_cpp = type_cpp_name(*etype);
                                    elem_sig_expr = "winrt::generic_arg{ "
                                        + std::to_string(is_iface ? 0 : (is_class ? 1 : 2))
                                        + ", \"" + (is_class ? (etype->ns + "." + etype->name) : std::string()) + "\", "
                                        + guid_literal(*eguid_src) + ", \"\" }";
                                    if (trace_coll)
                                        std::fprintf(stderr, "TRACE: elem typed %s (kind=%d)\n", (etype->ns + "." + etype->name).c_str(), (int)etype->kind);
                                }
                                else if (trace_coll)
                                {
                                    std::fprintf(stderr, "TRACE: elem MISS %s.%s find=%s has_guid=%d default=%s\n", ns.c_str(), name.c_str(),
                                        etype ? "ok" : "null",
                                        etype ? (int)etype->has_guid : -1,
                                        (etype && etype->default_interface && etype->default_interface->has_guid) ? "ok" : "none");
                                }
                            }
                        }
                    }
                    // The QI pattern is ALWAYS IVector, never the default
                    // interface's pattern: the methods emitted below (Append
                    // slot 13, GetAt slot 6, Clear slot 15, ...) are IVector<T>
                    // methods. The default interface may be IObservableVector<T>
                    // (ItemCollection), whose vtable has NO Append - dispatching
                    // IVector slots on that shape ran into an adjacent sub-object
                    // (garbage jmp target, AV in _guard_dispatch_icall_nop).
                    // DirectUI identity objects implement IVector<T> as a
                    // sub-object, so the closed IVector<T> QI succeeds and
                    // returns the correct vector shape.
                    pattern_lit = "winrt::impl::guid_ivector";
                }
                if (pattern_lit.empty() || elem_cpp.empty())
                {
                    // Fallback: the IInspectable vector (generic_arg kind 0,
                    // IInspectable IID - make_generic_iid emits the OS
                    // cinterface(IInspectable) spelling for it).
                    elem_cpp = "winrt::IInspectable";
                    elem_sig_expr = "winrt::generic_arg{ 0, \"\", winrt::iid_of<winrt::IInspectable>, \"\" }";
                    pattern_lit = "winrt::impl::guid_ivector";
                }
                // IVector<T> vtable: 6=GetAt, 7=GetView, 8=IndexOf, 9=GetMany,
                // 10=SetAt, 11=InsertAt, 12=RemoveAt, 13=Append,
                // 14=RemoveAtEnd, 15=Clear, 16=GetMany, 17=ReplaceAll.
                // The vector may be a tear-off unreachable from the identity
                // pointer, so every call QIs to the closed IVector<T> first
                // (never dispatch on m_unknown directly).
                w.write("\n    // IVector<T> collection methods. The vector is reached via\n");
                w.write("    // QI to the closed parameterized IVector<T> IID (computed\n");
                w.write("    // at compile time from metadata with the OS pinterface\n");
                w.write("    // SHA-1), since the framework may expose the vector as a\n");
                w.write("    // tear-off rather than on the identity pointer.\n");
                w.write("    static winrt::winrt_guid const& ivector_elem_iid()\n");
                w.write("    {\n");
                w.write("        static winrt::winrt_guid const value = winrt::impl::make_generic_iid(% , %);\n", pattern_lit, elem_sig_expr);
                w.write("        return value;\n");
                w.write("    }\n");
                w.write("    void* vector_abi() const\n");
                w.write("    {\n");
                w.write("        void* vec = nullptr;\n");
                w.write("        winrt::check_hresult(winrt::impl::query_interface(m_unknown.get(), ivector_elem_iid(), &vec));\n");
                w.write("        return vec;\n");
                w.write("    }\n");
                w.write("    void Append(% const& value) const\n", elem_cpp);
                w.write("    {\n");
                w.write("        void* vec = vector_abi();\n");
                w.write("        auto** vtbl = *reinterpret_cast<void***>(vec);\n");
                w.write("        reinterpret_cast<void(__stdcall*)(void*, void*)>(vtbl[13])(vec, winrt::impl::bind_in<%>{value});\n", elem_cpp);
                w.write("        winrt::IUnknown::release_raw(vec);\n");
                w.write("    }\n");
                w.write("    % GetAt(std::uint32_t index) const\n", elem_cpp);
                w.write("    {\n");
                w.write("        void* vec = vector_abi();\n");
                w.write("        auto** vtbl = *reinterpret_cast<void***>(vec);\n");
                w.write("        winrt::impl::arg_in_t<%> r{};\n", elem_cpp);
                w.write("        reinterpret_cast<std::int32_t(__stdcall*)(void*, std::uint32_t, void*)>(vtbl[6])(vec, index, reinterpret_cast<void*>(&r));\n");
                w.write("        winrt::IUnknown::release_raw(vec);\n");
                w.write("        return winrt::impl::collection_element<%>::from_abi(r);\n", elem_cpp);
                w.write("    }\n");
                w.write("    void Clear() const\n");
                w.write("    {\n");
                w.write("        void* vec = vector_abi();\n");
                w.write("        auto** vtbl = *reinterpret_cast<void***>(vec);\n");
                w.write("        reinterpret_cast<void(__stdcall*)(void*)>(vtbl[15])(vec);\n");
                w.write("        winrt::IUnknown::release_raw(vec);\n");
                w.write("    }\n");
            }
        }
    }

    w.write("};\n\n");
}

