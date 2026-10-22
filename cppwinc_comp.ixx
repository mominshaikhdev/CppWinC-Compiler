// cppwinc_comp.ixx — component writers
// Split from cppwinc.ixx for readability.
// This file is #included from cppwinc.ixx, not compiled independently.

// component writers

bool is_component_type(winmd::type_info const& type)
{
    std::error_code ec;
    auto const db = std::filesystem::weakly_canonical(type.db->path(), ec).string();
    // Component classes come only from the app .winmd(s) — every non-system
    // winmd in -input, whatever its position. Matching any input db made
    // every OS type a component class: the component module then absorbed
    // the whole SDK surface and imported it through non-existent winrt.*
    // modules.
    if (!settings.app_dbs.empty())
        return settings.app_dbs.contains(db);
    return settings.input_dbs.contains(db);
}

void write_component_class_impl(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const& metadata)
{
    std::string cpp_name = type_cpp_name(type);
    std::string module_ns = "winrt::" + std::string(type.ns);

    // impl base per own interface
    std::size_t impl_count = 0;
    for (auto const* iface : type.interfaces)
    {
        if (!iface || !iface->is_interface())
        {
            continue;
        }
        auto methods = flattened_methods(*iface, metadata);
        std::string impl_name = type.name + "_impl_" + iface->name;
        w.write("template <typename D>\n");
        // interfaces project at namespace scope (not nested in the
        // class), matching the component projection module
        std::string iface_cpp = type_cpp_name(*iface);
        w.write("struct % : winrt::impl::interface_impl_base<D, %>\n{\n", impl_name, iface_cpp);
        w.write("    using base = winrt::impl::interface_impl_base<D, %>;\n", iface_cpp);
        w.write("    explicit %(D& owner) : base(owner)\n    {\n", impl_name);
        for (std::size_t i = 0; i < methods.size(); ++i)
        {
            w.write("        base::m_methods[%] = reinterpret_cast<void*>(&thunk_%);\n", i, i);
        }
        w.write("    }\n\n");
        // thunks
        for (std::size_t i = 0; i < methods.size(); ++i)
        {
            auto const& method = *methods[i];
            w.write("    static std::int32_t __stdcall thunk_%(void* self", i);
            for (std::size_t p = 0; p < method.params.size(); ++p)
            {
                w.write(", ");
                auto const& pt = method.params[p].value;
                if (pt.kind == winmd::et_szarray)
                {
                    w.write("std::uint32_t a%_size, void* a%_data", p, p);
                }
                else if (pt.kind == winmd::et_valuetype && metadata.find(pt) && metadata.find(pt)->is_enum())
                {
                    w.write("std::int32_t a%", p);
                }
                else if (pt.kind >= winmd::et_boolean && pt.kind <= winmd::et_r8 && pt.kind != winmd::et_string)
                {
                    w.write("% a%", primitive_cpp(pt.kind), p);
                }
                else
                {
                    w.write("void* a%", p);
                }
            }
            if (method.ret.kind != winmd::et_void)
            {
                if (method.ret.kind == winmd::et_szarray)
                {
                    w.write(", std::uint32_t* r_size, void** r_data");
                }
                else if (method.ret.kind >= winmd::et_boolean && method.ret.kind <= winmd::et_r8 && method.ret.kind != winmd::et_string)
                {
                    w.write(", %* r", primitive_cpp(method.ret.kind));
                }
                else if (method.ret.kind == winmd::et_valuetype && metadata.find(method.ret) && metadata.find(method.ret)->is_enum())
                {
                    w.write(", std::int32_t* r");
                }
                else
                {
                    w.write(", void* r");
                }
            }
            w.write(")\n    {\n");
            // no CRTP downcast: the owner is recovered through the impl
            // base's base_from_abi, exactly like the runtime's own thunks.
            // The cast targets D (the template parameter), so the member
            // calls stay dependent and resolve when the class is complete.
            w.write("        auto& d = *static_cast<D*>(base::base_from_abi(self)->m_owner);\n");
    std::string ret_var = "result";
    for (auto const& p : method.params)
    {
        if (p.name == ret_var)
        {
            ret_var += "_";
        }
    }
            // The authored methods return values (they are not ABI
            // thunks), so the dispatch captures the return and the ABI
            // out-param is filled below; ret_out must NOT be appended
            // here (that is the consumer-side convention, where the
            // implementation is the OS taking out-params).
            if (method.ret.kind == winmd::et_void)
            {
                w.write("        d.%(", method.name);
            }
            else
            {
                w.write("        auto % = d.%(", ret_var, method.name);
            }
            for (std::size_t p = 0; p < method.params.size(); ++p)
            {
                if (p) w.write(", ");
                auto const& pt = method.params[p].value;
                if (pt.kind == winmd::et_szarray)
                {
                    w.write("winrt::com_array<%>::wrap(a%_data, a%_size)", mapper.map(pt.children.at(0)), p, p);
                }
                else if (pt.kind == winmd::et_valuetype && metadata.find(pt) && metadata.find(pt)->is_enum())
                {
                    w.write("static_cast<%>(a%)", mapper.map(pt), p);
                }
                else if (pt.kind >= winmd::et_boolean && pt.kind <= winmd::et_r8 && pt.kind != winmd::et_string)
                {
                    w.write("a%", p);
                }
                else if (pt.kind == winmd::et_string)
                {
                    // borrowed: the caller keeps the string alive for
                    // the call, so reinterpreting the ABI slot avoids
                    // from_abi's owning wrapper (whose destructor would
                    // release the caller's HSTRING)
                    w.write("*reinterpret_cast<winrt::hstring const*>(&a%)", p);
                }
                else
                {
                    // borrowed for the same reason: interface/generic
                    // parameters are the caller's references, so no
                    // owning wrapper is created (a from_abi temporary
                    // would Release them at the end of the thunk)
                    w.write("*reinterpret_cast<% const*>(&a%)", mapper.map(pt), p);
                }
            }
            w.write(");\n");
            if (method.ret.kind != winmd::et_void)
            {
                if (method.ret.kind == winmd::et_szarray)
                {
                    w.write("        *r_data = result.m_data;\n");
                    w.write("        *r_size = result.m_size;\n");
                    w.write("        result.m_data = nullptr;\n");
                    w.write("        result.m_size = 0;\n");
                }
                else if (method.ret.kind >= winmd::et_boolean && method.ret.kind <= winmd::et_r8 && method.ret.kind != winmd::et_string)
                {
                    w.write("        *r = result;\n");
                }
                else if (method.ret.kind == winmd::et_valuetype && metadata.find(method.ret) && metadata.find(method.ret)->is_enum())
                {
                    w.write("        *r = static_cast<int32_t>(result);\n");
                }
                else if (method.ret.kind == winmd::et_string)
                {
                    w.write("        *r = result.m_ptr;\n");
                    w.write("        result.m_ptr = nullptr;\n");
                }
                else if (method.ret.kind == winmd::et_valuetype)
                {
                    // move into the caller's slot: POD structs copy, and
                    // refcounted wrappers (e.g. event tokens projected as
                    // IInspectable) transfer their reference instead of
                    // double-owning it with the local result variable
                    w.write("        *reinterpret_cast<%*>(r) = std::move(result);\n", mapper.map(method.ret));
                }
                else
                {
                    w.write("        *r = result.m_ptr;\n");
                    w.write("        result.m_ptr = nullptr;\n");
                }
            }
            w.write("        return winrt::hr_ok;\n    }\n");
        }
        w.write("};\n\n");
        ++impl_count;
    }

    // class base
    std::string base_name = type.name + "_base";
    w.write("template <typename D>\n");
    w.write("struct % : winrt::implements<D, %>", base_name, cpp_name);
    for (auto const* iface : type.interfaces)
    {
        if (!iface || !iface->is_interface())
        {
            continue;
        }
        w.write(", %<D>", type.name + "_impl_" + iface->name);
    }
    w.write("\n{\n");
    w.write("    using % = winrt::implements<D, %>;\n", "impl_base", cpp_name);
    w.write("    winrt::com_ptr<void> m_base_object;\n\n");

    // ctors
    w.write("    %(D& owner", base_name);
    w.write(")\n        : impl_base(owner)");
    for (auto const* iface : type.interfaces)
    {
        if (!iface || !iface->is_interface())
        {
            continue;
        }
        w.write(", %<D>(owner)", type.name + "_impl_" + iface->name);
    }
    w.write("\n    {\n    }\n\n");

    // template get_abi: returns the first interface impl pointer (self)
    if (!type.interfaces.empty())
    {
        w.write("    template <typename I>\n    void* get_abi() const\n    {\n");
        w.write("        return static_cast<%<D> const*>(this)->get_abi();\n", type.name + "_impl_" + type.interfaces.front()->name);
        w.write("    }\n\n");
    }

    // query_interface + iids + classname
    w.write("    std::int32_t query_interface(winrt::winrt_guid const& iid, void** result) const\n    {\n");
    if (!type.interfaces.empty())
    {
        w.write("        if (iid == winrt::winrt_guid::from(IID_IUnknown) || iid == winrt::winrt_guid::from(IID_IInspectable))\n        {\n");
        w.write("            *result = static_cast<%<D> const*>(this)->get_abi();\n", type.name + "_impl_" + type.interfaces.front()->name);
        w.write("            const_cast<D*>(static_cast<D const*>(this))->add_ref();\n");
        w.write("            return winrt::hr_ok;\n        }\n");
    }
    for (auto const* iface : type.interfaces)
    {
        if (!iface || !iface->is_interface())
        {
            continue;
        }
        w.write("        if (iid == winrt::iid_of<%>)\n        {\n", type_cpp_name(*iface));
        w.write("            *result = static_cast<%<D> const*>(this)->get_abi();\n", type.name + "_impl_" + iface->name);
        w.write("            const_cast<D*>(static_cast<D const*>(this))->add_ref();\n");
        w.write("            return winrt::hr_ok;\n        }\n");
    }
    w.write("        if (m_base_object)\n        {\n");
    w.write("            GUID g = iid;\n");
    w.write("            HRESULT hr = winrt::IUnknown(m_base_object.get()).QueryInterface(g, result);\n");
    w.write("            if (hr >= 0) return winrt::hr_ok;\n        }\n");
    w.write("        return winrt::hr_no_interface;\n    }\n\n");

    w.write("    std::int32_t get_iids(std::uint32_t* count, winrt::winrt_guid** iids) const\n    {\n");
    w.write("        static winrt::winrt_guid const iid_list[] =\n        {\n");
    w.write("            winrt::winrt_guid::from(IID_IUnknown),\n            winrt::winrt_guid::from(IID_IInspectable),\n");
    for (auto const* iface : type.interfaces)
    {
        if (!iface || !iface->is_interface())
        {
            continue;
        }
        w.write("            winrt::iid_of<%>,\n", type_cpp_name(*iface));
    }
    w.write("        };\n");
    w.write("        *count = %;\n", 2 + static_cast<std::size_t>(std::ranges::count_if(type.interfaces, [](auto const* i) { return i && i->is_interface(); })));
    w.write("        auto* result = new winrt::winrt_guid[*count];\n");
    w.write("        std::copy(std::begin(iid_list), std::end(iid_list), result);\n");
    w.write("        *iids = result;\n        return winrt::hr_ok;\n    }\n\n");

    w.write("    std::int32_t get_class_name(winrt::hstring& name) const\n    {\n");
    w.write("        name = winrt::hstring(L\"%\");\n        return winrt::hr_ok;\n    }\n\n", type.full_name());

    w.write("    std::int32_t get_trust(std::int32_t* trust) const\n    {\n        *trust = 0;\n        return winrt::hr_ok;\n    }\n\n");

    w.write("    std::int32_t get_runtime_class_name(void* self, HSTRING* name) const\n    {\n");
    w.write("        winrt::hstring value(L\"%\");\n", type.full_name());
    w.write("        *name = reinterpret_cast<HSTRING>(value.m_ptr);\n");
    w.write("        value.m_ptr = nullptr;\n        return winrt::hr_ok;\n    }\n\n");

    w.write("};\n\n");
}

void write_component_projection(writer& w, winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const& metadata)
{
    // projected struct (like a platform class); methods are implemented in the .cpp
    w.write("export struct % : %<%>\n{\n", type.name, type.name + "_base", type.name);
    w.write("    using base_type = %<%>;\n", type.name + "_base", type.name);
    w.write("    static constexpr std::wstring_view class_name() noexcept { return L\"%\"; }\n", type.full_name());
    w.write("    winrt::com_ptr<void> m_unknown;\n");
    w.write("    mutable winrt::impl::iface_cache m_cache;\n\n");

    // default ctor creates the object directly
    w.write("    %()\n        : base_type(*this)\n    {\n", type.name);
    w.write("        m_unknown = winrt::com_ptr<void>::attach(static_cast<base_type*>(this)->template get_abi<winrt::IInspectable>());\n");
    w.write("        winrt::IUnknown(m_unknown.get()).AddRef();\n");
    w.write("    }\n\n");

    w.write("    %(void* base_object)\n        : base_type(*this)\n    {\n", type.name);
    w.write("        static_cast<base_type*>(this)->m_base_object = winrt::com_ptr<void>::attach(base_object);\n");
    w.write("        m_unknown = winrt::com_ptr<void>::attach(static_cast<base_type*>(this)->template get_abi<winrt::IInspectable>());\n");
    w.write("        winrt::IUnknown(m_unknown.get()).AddRef();\n");
    w.write("    }\n\n");

    w.write("    template <typename I>\n    void* iface() const\n    {\n        void* result = m_cache.get(winrt::iid_of<I>, m_unknown.get());\n        if (!result) throw winrt::hresult_error(winrt::hr_no_interface);\n        return result;\n    }\n\n");
    w.write("    template <typename I>\n    void* get_abi() const\n    {\n        return iface<I>();\n    }\n\n");

    // the impl base is non-copyable (com_object), so a move ctor is needed
    // to return instances by value; the base is freshly constructed and
    // m_unknown is transferred
    w.write("    %(%&& other) noexcept : base_type(*this)\n    {\n", type.name, type.name);
    w.write("        m_unknown = std::move(other.m_unknown);\n");
    w.write("    }\n\n");

    // factory-returned instances (e.g. ICalculatorStatics::Create) arrive
    // as a raw interface pointer; wrap it like the platform projection does
    w.write("    static % from_abi(void* ptr) noexcept\n    {\n", type.name);
    w.write("        % result;\n", type.name);
    w.write("        result.m_unknown = winrt::com_ptr<void>::attach(ptr);\n");
    w.write("        return result;\n    }\n\n");

    // implementation method declarations (defined in the per-class .cpp)
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
            std::string ret = ret_type(method.ret, mapper);
            w.write("    % %(", ret, method.name);
            for (std::size_t p = 0; p < method.params.size(); ++p)
            {
                if (p) w.write(", ");
                std::string name = method.params[p].name.empty() ? "arg" + std::to_string(p) : method.params[p].name;
                w.write("%", param_decl(method.params[p].value, name, mapper, (method.params[p].flags & winmd::param_out) != 0));
            }
            w.write(");\n");
        }
    }

    // statics (factory-interface members): declared on the class, implemented
    // by the author in the .cpp — e.g. ICalculatorStatics::Create -> Calculator::Create
    for (auto const* statics : type.statics)
    {
        if (!statics || !statics->is_interface())
        {
            continue;
        }
        for (auto const* method : flattened_methods(*statics, metadata))
        {
            std::string ret = ret_type(method->ret, mapper);
            w.write("    static % %(", ret, method->name);
            for (std::size_t p = 0; p < method->params.size(); ++p)
            {
                if (p) w.write(", ");
                std::string name = method->params[p].name.empty() ? "arg" + std::to_string(p) : method->params[p].name;
                w.write("%", param_decl(method->params[p].value, name, mapper, (method->params[p].flags & winmd::param_out) != 0));
            }
            w.write(");\n");
        }
    }

    w.write("};\n\n");
}

void write_component_cpp(winmd::type_info const& type, type_mapper const& mapper, winmd::metadata const& metadata, std::string const& output_folder)
{
    // Disabled: cppwinc only produces .ixx modules, never .cpp files
    (void)type; (void)mapper; (void)metadata; (void)output_folder;
    return;

    writer out;
    out.write("// % — generated by cppwinc v1.\n", type.name);
    out.write("// %\n", cppwinc::strings::copyright);
    out.write("\n");
    out.write("import %;\n", settings.component_name.empty() ? type.ns.substr(0, type.ns.find('.')) : settings.component_name);
    out.write("import std;\n");
    out.write("\n");
    out.write("namespace %\n{\n", cppwinc::ns_cpp(type.ns));
    // method stubs (implementation; the projection declares them in the component module)
    for (auto const* iface : type.interfaces)
    {
        if (!iface || !iface->is_interface())
        {
            continue;
        }
        auto methods = flattened_methods(*iface, metadata);
        for (auto const& method : methods)
        {
            if (method->is_static)
            {
                continue;
            }
            std::string ret = ret_type(method->ret, mapper);
            out.write("\n    % %::%(", ret, type.name, method->name);
            for (std::size_t p = 0; p < method->params.size(); ++p)
            {
                if (p) out.write(", ");
                std::string name = method->params[p].name.empty() ? "arg" + std::to_string(p) : method->params[p].name;
                out.write("%", param_decl(method->params[p].value, name, mapper, (method->params[p].flags & winmd::param_out) != 0));
            }
            out.write(")\n    {\n        throw winrt::hresult_error(winrt::hr_not_implemented);\n    }\n");
        }
    }
    // statics stubs (the author implements them, e.g. Calculator::Create)
    for (auto const* statics : type.statics)
    {
        if (!statics || !statics->is_interface())
        {
            continue;
        }
        for (auto const* method : flattened_methods(*statics, metadata))
        {
            std::string ret = ret_type(method->ret, mapper);
            out.write("\n    % %::%(", ret, type.name, method->name);
            for (std::size_t p = 0; p < method->params.size(); ++p)
            {
                if (p) out.write(", ");
                std::string name = method->params[p].name.empty() ? "arg" + std::to_string(p) : method->params[p].name;
                out.write("%", param_decl(method->params[p].value, name, mapper, (method->params[p].flags & winmd::param_out) != 0));
            }
            out.write(")\n    {\n        throw winrt::hresult_error(winrt::hr_not_implemented);\n    }\n");
        }
    }
    out.write("}\n");
    out.flush_to_file(std::filesystem::path(output_folder) / (type.name + ".cpp"));
}

