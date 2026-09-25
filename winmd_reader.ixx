// winmd_reader.ixx — module winmd_reader.
// A reader for Windows Runtime metadata (winmd) files, built against the
// ECMA-335 (Common Language Infrastructure) specification.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <ios>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <atomic>
#include <exception>
#include <mutex>
#include <cstdio>
#include <cstdlib>

export module winmd_reader;

namespace winmd
{
    export struct error : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    export using blob = std::span<std::byte const>;

    // Table and coded-index constants (ECMA-335 II.22)

    enum : std::uint32_t
    {
        table_module = 0x00,
        table_typeref = 0x01,
        table_typedef = 0x02,
        table_field = 0x04,
        table_methoddef = 0x06,
        table_param = 0x08,
        table_interfaceimpl = 0x09,
        table_memberref = 0x0A,
        table_constant = 0x0B,
        table_customattribute = 0x0C,
        table_declsecurity = 0x0E,
        table_classlayout = 0x0F,
        table_eventmap = 0x12,
        table_event = 0x14,
        table_propertymap = 0x15,
        table_property = 0x17,
        table_methodsemantics = 0x18,
        table_methodimpl = 0x19,
        table_moduleref = 0x1A,
        table_typespec = 0x1B,
        table_fieldrva = 0x1D,
        table_assembly = 0x20,
        table_assemblyref = 0x23,
        table_file = 0x26,
        table_exportedtype = 0x27,
        table_manifestresource = 0x28,
        table_nestedclass = 0x29,
        table_genericparam = 0x2A,
        table_methodspec = 0x2B,
        table_genericparamconstraint = 0x2C,
        table_count = 0x2D,
    };

    constexpr std::uint32_t coded_typedef_or_ref[] = { 2, 1, 27 };
    constexpr std::uint32_t coded_has_constant[] = { 4, 8, 23 };
    constexpr std::uint32_t coded_has_custom_attribute[] = { 6, 4, 1, 2, 8, 9, 10, 0, 14, 23, 20, 17, 12, 27, 32, 35, 38, 39, 40, 42, 44, 43 };
    constexpr std::uint32_t coded_has_field_marshal[] = { 4, 8 };
    constexpr std::uint32_t coded_has_decl_security[] = { 2, 6, 32 };
    constexpr std::uint32_t coded_member_ref_parent[] = { 2, 1, 26, 6, 27 };
    constexpr std::uint32_t coded_has_semantics[] = { 20, 23 };
    constexpr std::uint32_t coded_method_def_or_ref[] = { 6, 10 };
    constexpr std::uint32_t coded_member_forwarded[] = { 4, 6 };
    constexpr std::uint32_t coded_implementation[] = { 38, 35, 39 };
    constexpr std::uint32_t coded_custom_attribute_type[] = { 0, 0, 6, 10 };
    constexpr std::uint32_t coded_resolution_scope[] = { 0, 26, 35, 1 };
    constexpr std::uint32_t coded_type_or_method_def[] = { 2, 6 };

    export constexpr std::uint32_t attr_windows_runtime = 0x4000;
    export constexpr std::uint32_t attr_interface = 0x0020;
    export constexpr std::uint32_t attr_abstract = 0x0080;
    export constexpr std::uint32_t attr_sealed = 0x0100;
    export constexpr std::uint32_t attr_special_name = 0x0800;
    export constexpr std::uint32_t attr_rtspecial_name = 0x1000;
    export constexpr std::uint32_t attr_import = 0x1000;

    export constexpr std::uint32_t method_static = 0x0010;
    export constexpr std::uint32_t method_virtual = 0x0040;
    export constexpr std::uint32_t method_hidebysig = 0x0080;
    export constexpr std::uint32_t method_newslot = 0x0100;
    export constexpr std::uint32_t method_abstract = 0x0400;
    export constexpr std::uint32_t method_special_name = 0x0800;
    export constexpr std::uint32_t method_rtspecial_name = 0x1000;

    export constexpr std::uint32_t field_static = 0x0010;
    export constexpr std::uint32_t field_literal = 0x0040;
    export constexpr std::uint32_t field_special_name = 0x0200;

    export constexpr std::uint32_t param_in = 0x0001;
    export constexpr std::uint32_t param_out = 0x0002;

    // Element type codes
    export enum : std::uint8_t
    {
        et_end = 0x00,
        et_void = 0x01,
        et_boolean = 0x02,
        et_char = 0x03,
        et_i1 = 0x04,
        et_u1 = 0x05,
        et_i2 = 0x06,
        et_u2 = 0x07,
        et_i4 = 0x08,
        et_u4 = 0x09,
        et_i8 = 0x0A,
        et_u8 = 0x0B,
        et_r4 = 0x0C,
        et_r8 = 0x0D,
        et_string = 0x0E,
        et_ptr = 0x0F,
        et_byref = 0x10,
        et_valuetype = 0x11,
        et_class = 0x12,
        et_var = 0x13,
        et_array = 0x14,
        et_genericinst = 0x15,
        et_typedbyref = 0x16,
        et_i = 0x18,
        et_u = 0x19,
        et_fnptr = 0x1B,
        et_object = 0x1C,
        et_szarray = 0x1D,
        et_mvar = 0x1E,
        et_cmod_reqd = 0x1F,
        et_cmod_opt = 0x20,
        et_internal = 0x21,
        et_modifier = 0x40,
        et_sentinel = 0x41,
        et_pinned = 0x45,
        et_method = 0x50,
    };

    // Row reference

    export struct row
    {
        std::uint32_t table = table_count;
        std::uint32_t index = 0; // 1-based
        bool valid() const noexcept { return table < table_count && index != 0; }
        bool operator==(row const&) const = default;
        std::uint64_t key() const noexcept { return ((std::uint64_t)table << 32) | index; }
    };

    // Signature types

    class database;

    // generic arity suffix (`` `N ``) is not a valid C++ identifier character
    inline std::string strip_arity(std::string name)
    {
        auto pos = name.find('`');
        if (pos != std::string::npos)
        {
            name.erase(pos);
        }
        return name;
    }

    export struct type
    {
        std::uint8_t kind = et_end;      // element type code
        row type_row;                    // class_/valuetype (TypeDef/TypeRef/TypeSpec)
        database const* db = nullptr;    // database that contains type_row
        std::uint32_t var = 0;           // var/mvar index
        std::vector<type> args;          // genericinst args
        std::uint32_t rank = 0;          // array rank
        std::vector<type> children;      // 0 or 1 element (ptr/byref/szarray/array)
        std::vector<type> fn_params;     // fnptr parameters
        std::vector<type> fn_ret;        // fnptr return (0 or 1 element)
        bool pt_const = false;           // CModReqd(Const) modifier on pointee — C `const T*`

        bool is_simple() const noexcept { return kind < 0x11 || kind == et_string || kind == et_object || kind == et_void; }
    };

    export struct method_sig
    {
        std::uint8_t callconv = 0;
        std::uint32_t generic_params = 0;
        type ret;
        std::vector<type> params;
        std::vector<type> varargs;
    };

    // Custom attribute arguments

    export struct ca_arg
    {
        std::uint8_t kind = et_end;  // element type; 0x50 = System.Type (by name); 0x12 = enum
        std::int64_t i = 0;
        double d = 0;
        std::string s;
        std::string type_name;       // for enum: full enum name
        std::vector<ca_arg> arr;
    };

    // PE file

    export class pe_file
    {
        std::vector<std::byte> data_;
        struct section_info
        {
            std::uint32_t rva = 0;
            std::uint32_t vsize = 0;
            std::uint32_t raw = 0;
            std::uint32_t rawsize = 0;
        };
        std::vector<section_info> sections_;

        std::uint32_t rva_to_offset(std::uint32_t rva) const
        {
            for (auto const& s : sections_)
            {
                if (rva >= s.rva && rva - s.rva < s.vsize)
                {
                    std::uint32_t off = s.raw + (rva - s.rva);
                    if (off + (rva - s.rva) < s.raw + s.rawsize)
                    {
                        return off;
                    }
                    return s.raw + (rva - s.rva);
                }
            }
            throw error("invalid RVA in PE file");
        }

    public:
        explicit pe_file(std::filesystem::path const& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                throw error("cannot open file: " + path.string());
            }
            file.seekg(0, std::ios::end);
            auto size = file.tellg();
            if (size <= 0)
            {
                throw error("empty file: " + path.string());
            }
            data_.resize(static_cast<std::size_t>(size));
            file.seekg(0, std::ios::beg);
            file.read(reinterpret_cast<char*>(data_.data()), static_cast<std::streamsize>(data_.size()));

            if (data_.size() < 0x40 || read_u16(0) != 0x5A4D)
            {
                throw error("not a PE file: " + path.string());
            }
            std::uint32_t pe = read_u32(0x3C);
            if (pe + 4 > data_.size() || read_u32(pe) != 0x00004550)
            {
                throw error("invalid PE signature: " + path.string());
            }
            std::uint32_t num_sections = read_u16(pe + 6);
            std::uint32_t opt_size = read_u16(pe + 20);
            std::uint32_t sections = pe + 24 + opt_size;
            for (std::uint32_t i = 0; i < num_sections; ++i)
            {
                std::uint32_t p = sections + i * 40;
                section_info s;
                s.vsize = read_u32(p + 8);
                s.rva = read_u32(p + 12);
                s.rawsize = read_u32(p + 16);
                s.raw = read_u32(p + 20);
                sections_.push_back(s);
            }
        }

        std::uint32_t read_u16(std::uint32_t off) const
        {
            if (off + 2 > data_.size()) throw error("out of range");
            return (std::uint32_t)(std::to_integer<std::uint8_t>(data_[off])) |
                   ((std::uint32_t)(std::to_integer<std::uint8_t>(data_[off + 1])) << 8);
        }

        std::uint32_t read_u32(std::uint32_t off) const
        {
            if (off + 4 > data_.size()) throw error("out of range");
            return (std::uint32_t)(std::to_integer<std::uint8_t>(data_[off])) |
                   ((std::uint32_t)(std::to_integer<std::uint8_t>(data_[off + 1])) << 8) |
                   ((std::uint32_t)(std::to_integer<std::uint8_t>(data_[off + 2])) << 16) |
                   ((std::uint32_t)(std::to_integer<std::uint8_t>(data_[off + 3])) << 24);
        }

        blob at(std::uint32_t off, std::uint32_t size) const
        {
            if (off + size > data_.size()) throw error("out of range");
            return blob{ data_.data() + off, size };
        }

        blob rva_blob(std::uint32_t rva, std::uint32_t size) const
        {
            return at(rva_to_offset(rva), size);
        }
    };

    // Database (single winmd file)

    export class database
    {
        std::filesystem::path path_;
        pe_file pe_;

        // heaps
        blob strings_;
        blob blobs_;

        // tables
        std::uint32_t rows_[table_count] = {};
        std::size_t stride_[table_count] = {};
        blob tables_[table_count];

        // bucket caches, built once in the constructor

        // per-type monotonically computed range ends
        std::vector<std::uint32_t> type_method_end_;
        std::vector<std::uint32_t> type_field_end_;
        // per-type EventMap/PropertyMap row (at most one per type)
        std::vector<std::uint32_t> eventmap_row_by_type_;
        std::vector<std::uint32_t> propertymap_row_by_type_;
        // CustomAttribute rows keyed by parent row (table << 32 | row)
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> ca_by_parent_;
        // GenericParam rows keyed by owner (TypeDef or MethodDef)
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> gp_by_owner_;
        // GenericParamConstraint rows keyed by GenericParam row
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> constraints_by_gp_;
        // InterfaceImpl rows keyed by parent TypeDef
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> iimpl_by_type_;
        // MethodSemantics (flags, method) keyed by associated Event/Property row
        std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> semantics_by_assoc_;
        // Constant rows keyed by parent Field row
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> constants_by_field_;
        // MemberRef row keyed by referenced MethodDef row
        std::unordered_map<std::uint32_t, std::uint32_t> memberref_by_methoddef_;

        // heap index widths (1 = 4 bytes, 0 = 2 bytes)
        bool strings4_ = false;
        bool guids4_ = false;
        bool blobs4_ = false;

        // column descriptors
        enum class col_kind : std::uint8_t { u16, u32, string, guid, blob, simple, coded };
        struct col_spec
        {
            col_kind kind;
            std::uint32_t extra = 0;      // target table (simple) or tag table count (coded)
            std::uint32_t tag_bits = 0;
            std::uint32_t const* tag_tables = nullptr;
        };

        static std::span<col_spec const> table_columns(std::uint32_t table)
        {
            static const col_spec t_module[] = { {col_kind::u16}, {col_kind::string}, {col_kind::guid}, {col_kind::guid}, {col_kind::guid} };
            static const col_spec t_typeref[] = { {col_kind::coded, 4, 2, coded_resolution_scope}, {col_kind::string}, {col_kind::string} };
            static const col_spec t_typedef[] = { {col_kind::u32}, {col_kind::string}, {col_kind::string}, {col_kind::coded, 3, 2, coded_typedef_or_ref}, {col_kind::simple, table_field}, {col_kind::simple, table_methoddef} };
            static const col_spec t_fieldptr[] = { {col_kind::simple, table_field} };
            static const col_spec t_field[] = { {col_kind::u16}, {col_kind::string}, {col_kind::blob} };
            static const col_spec t_methodptr[] = { {col_kind::simple, table_methoddef} };
            static const col_spec t_methoddef[] = { {col_kind::u32}, {col_kind::u16}, {col_kind::u16}, {col_kind::string}, {col_kind::blob}, {col_kind::simple, table_param} };
            static const col_spec t_paramptr[] = { {col_kind::simple, table_param} };
            static const col_spec t_param[] = { {col_kind::u16}, {col_kind::u16}, {col_kind::string} };
            static const col_spec t_interfaceimpl[] = { {col_kind::simple, table_typedef}, {col_kind::coded, 3, 2, coded_typedef_or_ref} };
            static const col_spec t_memberref[] = { {col_kind::coded, 5, 3, coded_member_ref_parent}, {col_kind::string}, {col_kind::blob} };
            // Constant: Type (u16), Parent (coded HasConstant), Value (blob): no padding column
            static const col_spec t_constant[] = { {col_kind::u16}, {col_kind::coded, 3, 2, coded_has_constant}, {col_kind::blob} };
            static const col_spec t_customattribute[] = { {col_kind::coded, 22, 5, coded_has_custom_attribute}, {col_kind::coded, 4, 3, coded_custom_attribute_type}, {col_kind::blob} };
            static const col_spec t_fieldmarshal[] = { {col_kind::coded, 2, 1, coded_has_field_marshal}, {col_kind::blob} };
            static const col_spec t_declsecurity[] = { {col_kind::u16}, {col_kind::coded, 3, 2, coded_has_decl_security}, {col_kind::blob} };
            static const col_spec t_classlayout[] = { {col_kind::u16}, {col_kind::u32}, {col_kind::simple, table_typedef} };
            static const col_spec t_fieldlayout[] = { {col_kind::u32}, {col_kind::simple, table_field} };
            static const col_spec t_standalonesig[] = { {col_kind::blob} };
            static const col_spec t_eventmap[] = { {col_kind::simple, table_typedef}, {col_kind::simple, table_event} };
            static const col_spec t_eventptr[] = { {col_kind::simple, table_event} };
            static const col_spec t_event[] = { {col_kind::u16}, {col_kind::string}, {col_kind::coded, 3, 2, coded_typedef_or_ref} };
            static const col_spec t_propertymap[] = { {col_kind::simple, table_typedef}, {col_kind::simple, table_property} };
            static const col_spec t_propertyptr[] = { {col_kind::simple, table_property} };
            static const col_spec t_property[] = { {col_kind::u16}, {col_kind::string}, {col_kind::blob} };
            static const col_spec t_methodsemantics[] = { {col_kind::u16}, {col_kind::simple, table_methoddef}, {col_kind::coded, 2, 1, coded_has_semantics} };
            static const col_spec t_methodimpl[] = { {col_kind::simple, table_typedef}, {col_kind::coded, 2, 1, coded_method_def_or_ref}, {col_kind::coded, 2, 1, coded_method_def_or_ref} };
            static const col_spec t_moduleref[] = { {col_kind::string} };
            static const col_spec t_typespec[] = { {col_kind::blob} };
            static const col_spec t_implmap[] = { {col_kind::u16}, {col_kind::coded, 2, 1, coded_member_forwarded}, {col_kind::string}, {col_kind::simple, table_moduleref} };
            static const col_spec t_fieldrva[] = { {col_kind::u32}, {col_kind::simple, table_field} };
            static const col_spec t_enclog[] = { {col_kind::u32}, {col_kind::u32} };
            static const col_spec t_encmap[] = { {col_kind::u32} };
            static const col_spec t_assembly[] = { {col_kind::u32}, {col_kind::u16}, {col_kind::u16}, {col_kind::u16}, {col_kind::u16}, {col_kind::u32}, {col_kind::blob}, {col_kind::string}, {col_kind::string} };
            static const col_spec t_assemblyprocessor[] = { {col_kind::u32} };
            static const col_spec t_assemblyos[] = { {col_kind::u32}, {col_kind::u32}, {col_kind::u32} };
            static const col_spec t_assemblyref[] = { {col_kind::u16}, {col_kind::u16}, {col_kind::u16}, {col_kind::u16}, {col_kind::u32}, {col_kind::blob}, {col_kind::string}, {col_kind::string}, {col_kind::blob} };
            static const col_spec t_assemblyrefprocessor[] = { {col_kind::u32}, {col_kind::simple, table_assemblyref} };
            static const col_spec t_assemblyrefos[] = { {col_kind::u32}, {col_kind::u32}, {col_kind::u32}, {col_kind::simple, table_assemblyref} };
            static const col_spec t_file[] = { {col_kind::u32}, {col_kind::string}, {col_kind::blob} };
            static const col_spec t_exportedtype[] = { {col_kind::u32}, {col_kind::u32}, {col_kind::string}, {col_kind::string}, {col_kind::coded, 3, 2, coded_implementation} };
            static const col_spec t_manifestresource[] = { {col_kind::u32}, {col_kind::u32}, {col_kind::string}, {col_kind::coded, 3, 2, coded_implementation} };
            static const col_spec t_nestedclass[] = { {col_kind::simple, table_typedef}, {col_kind::simple, table_typedef} };
            static const col_spec t_genericparam[] = { {col_kind::u16}, {col_kind::u16}, {col_kind::coded, 2, 1, coded_type_or_method_def}, {col_kind::string} };
            static const col_spec t_methodspec[] = { {col_kind::coded, 2, 1, coded_method_def_or_ref}, {col_kind::blob} };
            static const col_spec t_genericparamconstraint[] = { {col_kind::simple, table_genericparam}, {col_kind::coded, 3, 2, coded_typedef_or_ref} };

            static const col_spec* all[] = {
                t_module, t_typeref, t_typedef, t_fieldptr, t_field, t_methodptr, t_methoddef,
                t_paramptr, t_param, t_interfaceimpl, t_memberref, t_constant, t_customattribute,
                t_fieldmarshal, t_declsecurity, t_classlayout, t_fieldlayout, t_standalonesig,
                t_eventmap, t_eventptr, t_event, t_propertymap, t_propertyptr, t_property,
                t_methodsemantics, t_methodimpl, t_moduleref, t_typespec, t_implmap, t_fieldrva,
                t_enclog, t_encmap, t_assembly, t_assemblyprocessor, t_assemblyos, t_assemblyref,
                t_assemblyrefprocessor, t_assemblyrefos, t_file, t_exportedtype, t_manifestresource,
                t_nestedclass, t_genericparam, t_methodspec, t_genericparamconstraint
            };
            static constexpr std::uint32_t col_counts[] = {
                5, 3, 6, 1, 3, 1, 6, 1, 3, 2, 3, 3, 3, 2, 3, 3, 2, 1, 2, 1, 3, 2, 1, 3, 3, 3, 1, 1, 4, 2, 2, 1, 9, 1, 3, 9, 2, 4, 3, 5, 4, 2, 4, 2, 2
            };
            if (table >= table_count) throw error("invalid table");
            return { all[table], col_counts[table] };
        }

        static std::uint32_t coded_size(std::span<col_spec const> cols, std::uint32_t const* rows, bool strings4, bool guids4, bool blobs4)
        {
            std::uint32_t size = 0;
            for (auto const& c : cols)
            {
                switch (c.kind)
                {
                case col_kind::u16: size += 2; break;
                case col_kind::u32: size += 4; break;
                case col_kind::string: size += strings4 ? 4 : 2; break;
                case col_kind::guid: size += guids4 ? 4 : 2; break;
                case col_kind::blob: size += blobs4 ? 4 : 2; break;
                case col_kind::simple: size += (rows[c.extra] > 0xFFFF) ? 4 : 2; break;
                case col_kind::coded:
                {
                    auto tag_rows = std::span{ c.tag_tables, c.extra } |
                        std::views::transform([&](std::uint32_t t) { return rows[t]; });
                    size += (std::ranges::max(tag_rows) >= (1u << (16 - c.tag_bits))) ? 4 : 2;
                    break;
                }
                }
            }
            return size;
        }

        // row data access (row is 1-based)
        std::uint32_t row_offset(std::uint32_t table, std::uint32_t row) const
        {
            // Malformed producer metadata (e.g. a coded index referencing a
            // row beyond the table's RowCount) must surface as an error, not
            // as an out-of-bounds read past the tables heap.
            constexpr std::uint32_t table_count = static_cast<std::uint32_t>(std::size(rows_));
            if (table >= table_count || row == 0 || row > rows_[table])
                throw error("bad row " + std::to_string(row) + " of table " + std::to_string(table) +
                            " (rows=" + std::to_string(table < table_count ? rows_[table] : 0) + ") in " + path_.string());
            return (row - 1) * static_cast<std::uint32_t>(stride_[table]);
        }

        std::uint32_t col_offset(std::uint32_t table, std::uint32_t col) const
        {
            std::uint32_t off = 0;
            auto cols = table_columns(table);
            for (std::uint32_t i = 0; i < col; ++i)
            {
                switch (cols[i].kind)
                {
                case col_kind::u16: off += 2; break;
                case col_kind::u32: off += 4; break;
                case col_kind::string: off += strings4_ ? 4 : 2; break;
                case col_kind::guid: off += guids4_ ? 4 : 2; break;
                case col_kind::blob: off += blobs4_ ? 4 : 2; break;
                case col_kind::simple: off += (rows_[cols[i].extra] > 0xFFFF) ? 4 : 2; break;
                case col_kind::coded:
                {
                    std::uint32_t max_rows = 0;
                    for (std::uint32_t j = 0; j < cols[i].extra; ++j)
                    {
                        max_rows = (std::max)(max_rows, rows_[cols[i].tag_tables[j]]);
                    }
                    off += (max_rows >= (1u << (16 - cols[i].tag_bits))) ? 4 : 2;
                    break;
                }
                }
            }
            return off;
        }

        std::uint32_t col_size(std::uint32_t table, std::uint32_t col) const
        {
            auto cols = table_columns(table);
            switch (cols[col].kind)
            {
            case col_kind::u16: return 2;
            case col_kind::u32: return 4;
            case col_kind::string: return strings4_ ? 4 : 2;
            case col_kind::guid: return guids4_ ? 4 : 2;
            case col_kind::blob: return blobs4_ ? 4 : 2;
            case col_kind::simple: return (rows_[cols[col].extra] > 0xFFFF) ? 4 : 2;
            case col_kind::coded:
            {
                std::uint32_t max_rows = 0;
                for (std::uint32_t j = 0; j < cols[col].extra; ++j)
                {
                    max_rows = (std::max)(max_rows, rows_[cols[col].tag_tables[j]]);
                }
                return (max_rows >= (1u << (16 - cols[col].tag_bits))) ? 4 : 2;
            }
            }
            return 2;
        }

        // little-endian read of an integral scalar (concept-constrained)
        template <std::integral T>
        static T read_le(blob b, std::uint32_t off) noexcept
        {
            T value{};
            std::memcpy(&value, b.data() + off, sizeof(T));
            return value;
        }

        std::uint32_t read_index(std::uint32_t table, std::uint32_t row, std::uint32_t col) const
        {
            std::uint32_t off = row_offset(table, row) + col_offset(table, col);
            auto data = tables_[table];
            if (col_size(table, col) == 4)
            {
                return read_le<std::uint32_t>(data, off);
            }
            return read_le<std::uint16_t>(data, off);
        }

        std::string_view heap_string(std::uint32_t index) const
        {
            if (index == 0) return {};
            if (index >= strings_.size()) throw error("bad string heap index " + std::to_string(index) + " size " + std::to_string(strings_.size()) + " in " + path_.string());            auto const* p = reinterpret_cast<char const*>(strings_.data() + index);
            std::size_t len = 0;
            while (index + len < strings_.size() && p[len] != '\0') ++len;
            return { p, len };
        }

        blob heap_blob(std::uint32_t index) const
        {
            if (index == 0) return {};
            auto [len, pos] = compressed_uint(blobs_, index);
            return blobs_.subspan(pos, len);
        }

        // compressed uint (ECMA-335 II.23.2)
        static std::pair<std::uint32_t, std::uint32_t> compressed_uint(blob b, std::uint32_t pos)
        {
            auto bad = [&]() -> error
            {
                return error("bad compressed uint pos=" + std::to_string(pos) + " size=" + std::to_string(b.size()));
            };
            if (pos >= b.size()) throw bad();
            std::uint8_t b0 = std::to_integer<std::uint8_t>(b[pos]);
            if ((b0 & 0x80) == 0) return { b0, pos + 1 };
            if ((b0 & 0x40) == 0)
            {
                if (pos + 1 >= b.size()) throw bad();
                return { ((std::uint32_t)(b0 & 0x3F) << 8) | std::to_integer<std::uint8_t>(b[pos + 1]), pos + 2 };
            }
            if (pos + 3 >= b.size()) throw bad();
            return { ((std::uint32_t)(b0 & 0x1F) << 24) |
                     ((std::uint32_t)std::to_integer<std::uint8_t>(b[pos + 1]) << 16) |
                     ((std::uint32_t)std::to_integer<std::uint8_t>(b[pos + 2]) << 8) |
                     std::to_integer<std::uint8_t>(b[pos + 3]), pos + 4 };
        }

        type parse_type(blob b, std::uint32_t& pos) const
        {
            type t;
            t.db = this;
            if (pos >= b.size()) throw error("bad signature: pos " + std::to_string(pos) + " size " + std::to_string(b.size()));
            t.kind = std::to_integer<std::uint8_t>(b[pos++]);

            switch (t.kind)
            {
            case et_valuetype:
            case et_class:
            {
                auto [idx, next] = compressed_uint(b, pos);
                pos = next;
                // coded TypeDefOrRef: table = tag_tables[tag], row = idx >> 2
                t.type_row = { coded_typedef_or_ref[idx & 0x3], idx >> 2 };
                break;
            }
            case et_genericinst:
            {
                std::uint8_t kv = std::to_integer<std::uint8_t>(b[pos++]);
                if (kv != et_class && kv != et_valuetype) throw error("bad genericinst");
                auto [idx, next] = compressed_uint(b, pos);
                pos = next;
                t.type_row = { coded_typedef_or_ref[idx & 0x3], idx >> 2 };
                auto [count, n2] = compressed_uint(b, pos);
                pos = n2;
                t.args.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    type a;
                    a.kind = et_end;
                    if (pos >= b.size()) throw error("bad signature");
                    a = parse_type(b, pos);
                    t.args.push_back(std::move(a));
                }
                break;
            }
            case et_var:
            case et_mvar:
            {
                auto [idx, next] = compressed_uint(b, pos);
                pos = next;
                t.var = idx;
                break;
            }
            case et_ptr:
            case et_szarray:
            {
                type child = parse_type(b, pos);
                t.children.push_back(std::move(child));
                break;
            }
            case et_byref:
            {
                // WinRT metadata encodes array out-returns as BYREF(SZARRAY);
                // keep the pointed-to type directly (transparent, like cmod).
                t = parse_type(b, pos);
                break;
            }
            case et_array:
            {
                type child = parse_type(b, pos);
                t.children.push_back(std::move(child));
                auto [rank, n2] = compressed_uint(b, pos);
                pos = n2;
                t.rank = rank;
                auto [num_sizes, n3] = compressed_uint(b, pos);
                pos = n3;
                for (std::uint32_t i = 0; i < num_sizes; ++i)
                {
                    auto [s, n4] = compressed_uint(b, pos);
                    pos = n4;
                }
                auto [num_lo, n5] = compressed_uint(b, pos);
                pos = n5;
                for (std::uint32_t i = 0; i < num_lo; ++i)
                {
                    auto [l, n6] = compressed_uint(b, pos);
                    pos = n6;
                }
                break;
            }
            case et_fnptr:
            {
                method_sig sig = parse_method_sig(b, pos);
                t.fn_ret.push_back(sig.ret);                t.fn_params = std::move(sig.params);
                break;
            }
            case et_cmod_reqd:
            case et_cmod_opt:
            {
                auto [idx, next] = compressed_uint(b, pos);
                pos = next;
                // Preserve the winversec const modifier (synthetic
                // Windows.Win32.Foundation.Const TypeRef): the projection
                // emits `T const*` from it. Other modifiers still resolve
                // transparently to the modified type.
                type inner = parse_type(b, pos);
                std::uint32_t ctable = coded_typedef_or_ref[idx & 0x3];
                std::uint32_t cindex = idx >> 2;
                if (ctable == table_typeref && cindex > 0 && cindex <= rows(table_typeref))
                {
                    std::string cns = std::string(get_string(table_typeref, cindex, 2));
                    std::string cname = strip_arity(std::string(get_string(table_typeref, cindex, 1)));
                    if (cns == "Windows.Win32.Foundation" && cname == "Const")
                    {
                        inner.pt_const = true;
                    }
                }
                t = std::move(inner); // modifiers attach to following type; keep following type
                break;
            }
            default:
                break;
            }
            return t;
        }

        method_sig parse_method_sig(blob b, std::uint32_t& pos) const
        {
            if (pos >= b.size()) return {};
            method_sig sig;
            sig.callconv = std::to_integer<std::uint8_t>(b[pos++]);            if (sig.callconv & 0x10) // generic
            {
                auto [gc, next] = compressed_uint(b, pos);
                pos = next;
                sig.generic_params = gc;
            }
            auto [pc, n1] = compressed_uint(b, pos);
            pos = n1;
            sig.ret = parse_type(b, pos);
            sig.params.reserve(pc);
            for (std::uint32_t i = 0; i < pc; ++i)
            {
                if (pos < b.size() && std::to_integer<std::uint8_t>(b[pos]) == et_sentinel)
                {
                    ++pos;
                    while (pos < b.size())
                    {
                        sig.varargs.push_back(parse_type(b, pos));
                    }
                    break;
                }
                sig.params.push_back(parse_type(b, pos));
            }
            return sig;
        }

        method_sig parse_field_sig(blob b) const
        {
            std::uint32_t pos = 0;
            if (pos >= b.size() || std::to_integer<std::uint8_t>(b[pos]) != 0x06)
            {
                throw error("bad field signature");
            }
            ++pos;
            method_sig sig;
            sig.ret = parse_type(b, pos);
            return sig;
        }

        method_sig parse_property_sig(blob b) const
        {
            std::uint32_t pos = 0;
            if (pos >= b.size() || std::to_integer<std::uint8_t>(b[pos]) != 0x08)
            {
                throw error("bad property signature");
            }
            ++pos;
            auto [pc, n1] = compressed_uint(b, pos);
            pos = n1;
            method_sig sig;
            sig.ret = parse_type(b, pos);
            sig.params.reserve(pc);
            for (std::uint32_t i = 0; i < pc; ++i)
            {
                sig.params.push_back(parse_type(b, pos));
            }
            return sig;
        }

        void load_tables(blob til_stream)
        {
            if (til_stream.size() < 24) throw error("bad #~ stream");
            std::uint32_t heap_sizes = std::to_integer<std::uint8_t>(til_stream[6]);
            strings4_ = (heap_sizes & 0x01) != 0;
            guids4_ = (heap_sizes & 0x02) != 0;
            blobs4_ = (heap_sizes & 0x04) != 0;

            auto read64 = [&](std::uint32_t off)
            {
                std::uint64_t v = 0;
                for (int i = 0; i < 8; ++i)
                {
                    v |= (std::uint64_t)std::to_integer<std::uint8_t>(til_stream[off + i]) << (8 * i);
                }
                return v;
            };

            std::uint64_t valid = read64(8);
            std::uint32_t pos = 24;

            for (std::uint32_t t = 0; t < table_count; ++t)
            {
                if (!(valid & (1ull << t))) continue;
                rows_[t] = (std::uint32_t)((std::uint64_t)std::to_integer<std::uint8_t>(til_stream[pos])) |
                           ((std::uint32_t)std::to_integer<std::uint8_t>(til_stream[pos + 1]) << 8) |
                           ((std::uint32_t)std::to_integer<std::uint8_t>(til_stream[pos + 2]) << 16) |
                           ((std::uint32_t)std::to_integer<std::uint8_t>(til_stream[pos + 3]) << 24);
                pos += 4;
            }

            for (std::uint32_t t = 0; t < table_count; ++t)
            {
                if (!(valid & (1ull << t))) continue;
                auto cols = table_columns(t);
                stride_[t] = coded_size(cols, rows_, strings4_, guids4_, blobs4_);
                tables_[t] = til_stream.subspan(pos, rows_[t] * stride_[t]);
                pos += static_cast<std::uint32_t>(rows_[t] * stride_[t]);
            }
        }

    public:
        explicit database(std::filesystem::path const& path)
            : path_(path), pe_(path)
        {
            // locate CLI header
            std::uint32_t pe = pe_.read_u32(0x3C);
            std::uint32_t magic = pe_.read_u16(pe + 24);
            std::uint32_t num_dirs = pe_.read_u32(pe + (magic == 0x20B ? 132 : 116));
            std::uint32_t dirs = pe + (magic == 0x20B ? 136 : 120);
            if (num_dirs <= 14) throw error("no CLI header in: " + path.string());
            std::uint32_t cli_rva = pe_.read_u32(dirs + 14 * 8);
            std::uint32_t cli_size = pe_.read_u32(dirs + 14 * 8 + 4);
            if (!cli_rva || cli_size < 72) throw error("no CLI header in: " + path.string());
            auto cli = pe_.rva_blob(cli_rva, cli_size);
            if (cli.size() < 16) throw error("bad CLI header in: " + path.string());
            auto rva_u32 = [&](std::uint32_t off)
            {
                return (std::uint32_t)(std::to_integer<std::uint8_t>(cli[off])) |
                       ((std::uint32_t)(std::to_integer<std::uint8_t>(cli[off + 1])) << 8) |
                       ((std::uint32_t)(std::to_integer<std::uint8_t>(cli[off + 2])) << 16) |
                       ((std::uint32_t)(std::to_integer<std::uint8_t>(cli[off + 3])) << 24);
            };
            std::uint32_t md_rva = rva_u32(8);
            std::uint32_t md_size = rva_u32(12);
            if (!md_rva) throw error("no metadata in: " + path.string());

            auto md = pe_.rva_blob(md_rva, md_size);
            auto sig = [&](std::uint32_t off) -> std::uint32_t
            {
                if (off + 4 > md.size()) throw error("bad metadata in: " + path.string());
                return (std::uint32_t)(std::to_integer<std::uint8_t>(md[off])) |
                       ((std::uint32_t)(std::to_integer<std::uint8_t>(md[off + 1])) << 8) |
                       ((std::uint32_t)(std::to_integer<std::uint8_t>(md[off + 2])) << 16) |
                       ((std::uint32_t)(std::to_integer<std::uint8_t>(md[off + 3])) << 24);
            };
            auto u16 = [&](std::uint32_t off) -> std::uint32_t
            {
                return (std::uint32_t)(std::to_integer<std::uint8_t>(md[off])) |
                       ((std::uint32_t)(std::to_integer<std::uint8_t>(md[off + 1])) << 8);
            };
            if (sig(0) != 0x424A5342) throw error("bad metadata signature in: " + path.string());
            std::uint32_t ver_len = sig(12);
            std::uint32_t stream_pos = 16 + ver_len;
            std::uint32_t stream_count = u16(stream_pos + 2);
            stream_pos += 4;
            for (std::uint32_t i = 0; i < stream_count; ++i)
            {
                std::uint32_t off = sig(stream_pos);
                std::uint32_t size = sig(stream_pos + 4);
                std::uint32_t name_pos = stream_pos + 8;
                std::uint32_t name_len = 0;
                while (name_pos + name_len < md.size() && md[name_pos + name_len] != std::byte{0})
                {
                    ++name_len;
                }
                std::string_view name(reinterpret_cast<char const*>(md.data() + name_pos), name_len);
                stream_pos += 8 + name_len + 1;
                stream_pos = (stream_pos + 3) & ~3u;

                if (name == "#~")
                {
                    load_tables(md.subspan(off, size));
                }
                else if (name == "#Strings")
                {
                    strings_ = md.subspan(off, size);
                }
                else if (name == "#Blob")
                {
                    blobs_ = md.subspan(off, size);
                }
            }

            if (tables_[table_typedef].empty() && rows_[table_typedef] != 0)
            {
                throw error("bad metadata in: " + path.string());
            }

            build_indexes();
        }

        std::filesystem::path const& path() const noexcept { return path_; }
        std::uint32_t rows(std::uint32_t table) const noexcept { return rows_[table]; }

        // bucket index accessors

        std::uint32_t type_method_end(std::uint32_t tr) const noexcept { return type_method_end_[tr]; }
        std::uint32_t type_field_end(std::uint32_t tr) const noexcept { return type_field_end_[tr]; }
        std::uint32_t eventmap_row(std::uint32_t tr) const noexcept { return eventmap_row_by_type_[tr]; }
        std::uint32_t propertymap_row(std::uint32_t tr) const noexcept { return propertymap_row_by_type_[tr]; }

        std::vector<std::uint32_t> const* gp_by_owner(row const& owner) const noexcept
        {
            auto it = gp_by_owner_.find(owner.key());
            return it == gp_by_owner_.end() ? nullptr : &it->second;
        }
        std::vector<std::uint32_t> const* constraints_by_gp(std::uint32_t g) const noexcept
        {
            auto it = constraints_by_gp_.find(g);
            return it == constraints_by_gp_.end() ? nullptr : &it->second;
        }
        std::vector<std::uint32_t> const* iimpl_by_type(std::uint32_t tr) const noexcept
        {
            auto it = iimpl_by_type_.find(tr);
            return it == iimpl_by_type_.end() ? nullptr : &it->second;
        }
        std::vector<std::pair<std::uint32_t, std::uint32_t>> const* semantics_by_assoc(row const& assoc) const noexcept
        {
            auto it = semantics_by_assoc_.find(assoc.key());
            return it == semantics_by_assoc_.end() ? nullptr : &it->second;
        }
        std::vector<std::uint32_t> const* constants_by_field(std::uint32_t f) const noexcept
        {
            auto it = constants_by_field_.find(f);
            return it == constants_by_field_.end() ? nullptr : &it->second;
        }
        std::uint32_t memberref_by_methoddef(std::uint32_t m) const noexcept
        {
            auto it = memberref_by_methoddef_.find(m);
            return it == memberref_by_methoddef_.end() ? 0 : it->second;
        }

    private:
        // linear-time pass over the row tables to build the bucket indexes
        void build_indexes()
        {
            std::uint32_t ntd = rows(table_typedef);
            type_method_end_.assign(ntd + 1, 0);
            type_field_end_.assign(ntd + 1, 0);
            eventmap_row_by_type_.assign(ntd + 1, 0);
            propertymap_row_by_type_.assign(ntd + 1, 0);

            // method/field ranges: ECMA-335 — a type owns [T.list, T_next.list).
            // Equal list values on consecutive rows mean the EARLIER rows own
            // nothing (empty types, e.g. DECLARE_HANDLE classes like
            // HSTRING_BUFFER / PACKAGEDEPENDENCY_CONTEXT that precede their
            // first function carrier). Grouping equal-value runs and giving
            // the whole run the run-after end was wrong: it handed every
            // empty type the next real method as a phantom member, which
            // (a) blocked the opaque-handle void* emission in the driver and
            // (b) leaked foreign methods into signatures.
            for (std::uint32_t tr = 1; tr <= ntd; ++tr)
            {
                std::uint32_t ml = get_simple(table_typedef, tr, 5);
                std::uint32_t end = (tr < ntd) ? get_simple(table_typedef, tr + 1, 5)
                                               : rows(table_methoddef) + 1;
                type_method_end_[tr] = (end > ml) ? end : ml; // guard: end must not precede start
            }
            for (std::uint32_t tr = 1; tr <= ntd; ++tr)
            {
                std::uint32_t fl = get_simple(table_typedef, tr, 4);
                std::uint32_t end = (tr < ntd) ? get_simple(table_typedef, tr + 1, 4)
                                               : rows(table_field) + 1;
                type_field_end_[tr] = (end > fl) ? end : fl;
            }

            for (std::uint32_t em = 1; em <= rows(table_eventmap); ++em)
            {
                std::uint32_t tr = get_simple(table_eventmap, em, 0);
                if (tr >= 1 && tr <= ntd) eventmap_row_by_type_[tr] = em;
            }
            for (std::uint32_t pm = 1; pm <= rows(table_propertymap); ++pm)
            {
                std::uint32_t tr = get_simple(table_propertymap, pm, 0);
                if (tr >= 1 && tr <= ntd) propertymap_row_by_type_[tr] = pm;
            }

            for (std::uint32_t r = 1; r <= rows(table_customattribute); ++r)
            {
                ca_by_parent_[get_coded(table_customattribute, r, 0).key()].push_back(r);
            }
            for (std::uint32_t g = 1; g <= rows(table_genericparam); ++g)
            {
                gp_by_owner_[get_coded(table_genericparam, g, 2).key()].push_back(g);
            }
            for (std::uint32_t c = 1; c <= rows(table_genericparamconstraint); ++c)
            {
                constraints_by_gp_[get_simple(table_genericparamconstraint, c, 0)].push_back(c);
            }
            for (std::uint32_t i = 1; i <= rows(table_interfaceimpl); ++i)
            {
                iimpl_by_type_[get_simple(table_interfaceimpl, i, 0)].push_back(i);
            }
            for (std::uint32_t ms = 1; ms <= rows(table_methodsemantics); ++ms)
            {
                row assoc = get_coded(table_methodsemantics, ms, 2);
                if (assoc.table == table_event || assoc.table == table_property)
                {
                    semantics_by_assoc_[assoc.key()].emplace_back(get_u32(table_methodsemantics, ms, 0), get_simple(table_methodsemantics, ms, 1));
                }
            }
            for (std::uint32_t c = 1; c <= rows(table_constant); ++c)
            {
                row parent = get_coded(table_constant, c, 1);
                if (parent.table == table_field)
                {
                    constants_by_field_[parent.index].push_back(c);
                }
            }
            for (std::uint32_t r = 1; r <= rows(table_memberref); ++r)
            {
                row mref = get_coded(table_memberref, r, 1);
                if (mref.table == table_methoddef)
                {
                    memberref_by_methoddef_[mref.index] = r;
                }
            }
        }

    public:

        // generic column access (row is 1-based)

        std::uint32_t get_u32(std::uint32_t table, std::uint32_t row, std::uint32_t col) const
        {
            return read_index(table, row, col);
        }

        std::string_view get_string(std::uint32_t table, std::uint32_t row, std::uint32_t col) const
        {
            try
            {
                return heap_string(read_index(table, row, col));
            }
            catch (error const& e)
            {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%08X", table);
                throw error(std::string(e.what()) + " [table=0x" + buf + " row=" + std::to_string(row) + " col=" + std::to_string(col) + "]");
            }
        }

        blob get_blob(std::uint32_t table, std::uint32_t row, std::uint32_t col) const
        {
            return heap_blob(read_index(table, row, col));
        }

        std::uint32_t get_simple(std::uint32_t table, std::uint32_t row, std::uint32_t col) const
        {
            return read_index(table, row, col);
        }

        row get_coded(std::uint32_t table, std::uint32_t row, std::uint32_t col) const
        {
            auto cols = table_columns(table);
            std::uint32_t value = read_index(table, row, col);
            std::uint32_t tag = value & ((1u << cols[col].tag_bits) - 1);
            std::uint32_t index = value >> cols[col].tag_bits;
            if (tag >= cols[col].extra || !cols[col].tag_tables[tag])
            {
                return {};
            }
            return { cols[col].tag_tables[tag], index };
        }

        // custom attributes

        std::vector<std::pair<row, blob>> custom_attributes(row const& parent) const
        {
            std::vector<std::pair<row, blob>> result;
            auto it = ca_by_parent_.find(parent.key());
            if (it == ca_by_parent_.end())
            {
                return result;
            }
            result.reserve(it->second.size());
            for (auto const& r : it->second)
            {
                result.emplace_back(get_coded(table_customattribute, r, 1), get_blob(table_customattribute, r, 2));
            }
            return result;
        }

        // attribute full name from CustomAttributeType (MethodDef/MemberRef)
        std::string attribute_type_name(row const& ca_type) const
        {
            if (ca_type.table == table_methoddef)
            {
                // ctor in this database; find the MemberRef that references it
                std::uint32_t mref = memberref_by_methoddef(ca_type.index);
                if (mref)
                {
                    row cls = get_coded(table_memberref, mref, 0);
                    return type_ref_name(cls);
                }
                return {};
            }
            if (ca_type.table == table_memberref)
            {
                row cls = get_coded(table_memberref, ca_type.index, 0);
                return type_ref_name(cls);
            }
            return {};
        }

        std::string type_def_name(std::uint32_t row) const
        {
            if (!row) return {};
            std::string ns = std::string(get_string(table_typedef, row, 2));
            std::string name = strip_arity(std::string(get_string(table_typedef, row, 1)));
            if (ns.empty()) return name;
            return ns + "." + name;
        }

        std::string type_ref_name(row const& r) const
        {
            if (!r.valid()) return {};
            if (r.table == table_typedef)
            {
                return type_def_name(r.index);
            }
            if (r.table == table_typeref)
            {
                std::string ns = std::string(get_string(table_typeref, r.index, 2));
                std::string name = strip_arity(std::string(get_string(table_typeref, r.index, 1)));
                if (ns.empty()) return name;
                return ns + "." + name;
            }
            if (r.table == table_typespec)
            {
                blob b = get_blob(table_typespec, r.index, 0);
                std::uint32_t pos = 0;
                type t = parse_type(b, pos);
                return type_name(t);
            }
            return {};
        }

        std::string type_name(type const& t) const
        {
            if (t.kind == et_class || t.kind == et_valuetype)
            {
                return type_ref_name(t.type_row);
            }
            if (t.kind == et_genericinst)
            {
                std::string s = type_ref_name(t.type_row);
                s += "<";
                for (std::size_t i = 0; i < t.args.size(); ++i)
                {
                    if (i) s += ", ";
                    s += type_name(t.args[i]);
                }
                s += ">";
                return s;
            }
            static constexpr std::string_view names[] = {
                "", "void", "bool", "char", "int8_t", "uint8_t", "int16_t", "uint16_t",
                "int32_t", "uint32_t", "int64_t", "uint64_t", "float", "double",
                "winrt::hstring", "", "", "", "", "", "", "IReference?", "", "",
                "winrt::Windows::Foundation::IInspectable"
            };
            if (t.kind == et_szarray) return type_name(t.children[0]) + "[]";
            if (t.kind == et_var || t.kind == et_mvar) return std::string("T") + std::to_string(t.var);
            if (t.kind < std::size(names)) return std::string(names[t.kind]);
            return {};
        }

        // parse signature blobs
        method_sig parse_method_signature(blob b) const
        {
            std::uint32_t pos = 0;
            try
            {
                return parse_method_sig(b, pos);
            }
            catch (error const& e)
            {
                std::string hex;
                for (std::size_t i = 0; i < b.size(); ++i)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "%02X ", (unsigned)std::to_integer<unsigned char>(b[i]));
                    hex += buf;
                }
                throw error(std::string(e.what()) + " blob=[" + hex + "]");
            }
        }

        type parse_field_type(blob b) const
        {
            return parse_field_sig(b).ret;
        }

        // Default interface of a runtime class, preserving generic
        // instantiation info: re-reads the DefaultAttribute-marked
        // InterfaceImpl's TypeRef/TypeDef/TypeSpec signature (resolve() on a
        // TypeSpec drops the generic args, so the emitter needs this to see
        // "IVector`1" + <PathFigure>). For "class C : [default] IVector<T>"
        // this yields generic_name "Windows.Foundation.Collections.IVector`1"
        // with one arg — the closed-pinterface pattern + args needed to
        // compute the collection's instance IID. Returns false when the class
        // has no default interface or it is not a generic instantiation.
        bool default_iface_sig(std::uint32_t typedef_row,
                               std::string& generic_name,
                               std::vector<type>& args) const
        {
            std::vector<std::uint32_t> const* ii = iimpl_by_type(typedef_row);
            if (!ii) return false;
            for (std::uint32_t i : *ii)
            {
                bool is_default = false;
                for (auto const& [ca_type, value] : custom_attributes({ table_interfaceimpl, i }))
                {
                    if (attribute_type_name(ca_type) == "Windows.Foundation.Metadata.DefaultAttribute")
                    {
                        is_default = true;
                        break;
                    }
                }
                if (!is_default) continue;
                row ir = get_coded(table_interfaceimpl, i, 1);
                try
                {
                    if (ir.table == table_typespec)
                    {
                        blob b = get_blob(table_typespec, ir.index, 0);
                        std::uint32_t pos = 0;
                        type t = parse_type(b, pos);
                        if (t.kind == et_genericinst)
                        {
                            generic_name = type_ref_name(t.type_row);
                            args = t.args;
                            return true;
                        }
                    }
                }
                catch (...)
                {
                    return false;
                }
                return false;
            }
            return false;
        }

        type parse_property_type(blob b) const
        {
            return parse_property_sig(b).ret;
        }

        method_sig parse_property_signature(blob b) const
        {
            return parse_property_sig(b);
        }

        // custom attribute blob parsing

        static std::pair<std::string, std::uint32_t> ser_string(blob b, std::uint32_t pos)
        {
            if (pos >= b.size()) throw error("bad attribute blob");
            std::uint8_t b0 = std::to_integer<std::uint8_t>(b[pos]);
            if (b0 == 0xFF) return { {}, pos + 1 };
            if ((b0 & 0x80) == 0)
            {
                // 1-byte compressed: length = b0 (7 bits)
                std::string s(reinterpret_cast<char const*>(b.data() + pos + 1), b0);
                return { s, pos + 1 + b0 };
            }
            if ((b0 & 0xC0) == 0x80)
            {
                // 2-byte compressed: length = (b0 & 0x3F) << 8 | b1 (14 bits)
                std::uint32_t len = (b0 & 0x3F) << 8;
                if (pos + 1 >= b.size()) throw error("bad attribute blob");
                len |= std::to_integer<std::uint8_t>(b[pos + 1]);
                std::string s(reinterpret_cast<char const*>(b.data() + pos + 2), len);
                return { s, pos + 2 + len };
            }
            // 4-byte compressed: length = (b0 & 0x1F) << 24 | b1 << 16 | b2 << 8 | b3 (29 bits)
            if (pos + 3 >= b.size()) throw error("bad attribute blob");
            std::uint32_t len = (b0 & 0x1F) << 24;
            len |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[pos + 1])) << 16;
            len |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[pos + 2])) << 8;
            len |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[pos + 3]));
            std::string s(reinterpret_cast<char const*>(b.data() + pos + 4), len);
            return { s, pos + 4 + len };
        }

        static ca_arg read_fixed_value(blob b, std::uint32_t& pos, std::uint8_t kind)
        {
            ca_arg arg;
            arg.kind = kind;
            switch (kind)
            {
            case et_boolean: arg.i = std::to_integer<std::uint8_t>(b[pos]); pos += 1; break;
            case et_char: arg.i = (std::uint16_t)(std::to_integer<std::uint8_t>(b[pos]) | (std::to_integer<std::uint8_t>(b[pos + 1]) << 8)); pos += 2; break;
            case et_i1: arg.i = (std::int8_t)std::to_integer<std::uint8_t>(b[pos]); pos += 1; break;
            case et_u1: arg.i = std::to_integer<std::uint8_t>(b[pos]); pos += 1; break;
            case et_i2: arg.i = (std::int16_t)(std::to_integer<std::uint8_t>(b[pos]) | (std::to_integer<std::uint8_t>(b[pos + 1]) << 8)); pos += 2; break;
            case et_u2: arg.i = (std::uint16_t)(std::to_integer<std::uint8_t>(b[pos]) | (std::to_integer<std::uint8_t>(b[pos + 1]) << 8)); pos += 2; break;
            case et_i4: arg.i = (std::int32_t)read_u32_raw(b, pos); pos += 4; break;
            case et_u4: arg.i = read_u32_raw(b, pos); pos += 4; break;
            case et_i8: arg.i = read_u64_raw(b, pos); pos += 8; break;
            case et_u8: arg.i = (std::int64_t)read_u64_raw(b, pos); pos += 8; break;
            case et_r4:
            {
                std::uint32_t bits = read_u32_raw(b, pos);
                pos += 4;
                float f;
                std::memcpy(&f, &bits, 4);
                arg.d = f;
                break;
            }
            case et_r8:
            {
                std::uint64_t bits = read_u64_raw(b, pos);
                pos += 8;
                double d;
                std::memcpy(&d, &bits, 8);
                arg.d = d;
                break;
            }
            case et_string:
            {
                auto [s, next] = ser_string(b, pos);
                pos = next;
                arg.s = std::move(s);
                break;
            }
            case 0x50: // System.Type
            {
                auto [s, next] = ser_string(b, pos);
                pos = next;
                arg.s = std::move(s);
                arg.kind = 0x50;
                break;
            }
            case 0x12: // enum: SerString full name + u32
            {
                auto [s, next] = ser_string(b, pos);
                pos = next;
                arg.type_name = std::move(s);
                arg.i = read_u32_raw(b, pos);
                pos += 4;
                arg.kind = 0x12;
                break;
            }
            case et_szarray:
            {
                // count then elements of the (unknown) element type; element type
                // follows in blob per spec — but for our attributes arrays carry
                // their element kind byte before the count
                if (pos < b.size())
                {
                    arg.kind = std::to_integer<std::uint8_t>(b[pos++]);
                }
                std::uint32_t count = read_u32_raw(b, pos);
                pos += 4;
                arg.arr.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    arg.arr.push_back(read_fixed_value(b, pos, arg.kind));
                }
                break;
            }
            default:
                throw error("unsupported attribute value kind");
            }
            return arg;
        }

        static std::uint32_t read_u32_raw(blob b, std::uint32_t pos)
        {
            return (std::uint32_t)(std::to_integer<std::uint8_t>(b[pos])) |
                   ((std::uint32_t)(std::to_integer<std::uint8_t>(b[pos + 1])) << 8) |
                   ((std::uint32_t)(std::to_integer<std::uint8_t>(b[pos + 2])) << 16) |
                   ((std::uint32_t)(std::to_integer<std::uint8_t>(b[pos + 3])) << 24);
        }

        static std::uint64_t read_u64_raw(blob b, std::uint32_t pos)
        {
            return (std::uint64_t)read_u32_raw(b, pos) | ((std::uint64_t)read_u32_raw(b, pos + 4) << 32);
        }

        // Parse fixed args given the ctor's parameter types.
        static std::vector<ca_arg> parse_fixed_args(blob b, std::vector<type> const& ctor_params)
        {
            std::vector<ca_arg> result;
            if (b.size() < 2) return result;
            std::uint32_t pos = 2; // skip prolog
            for (auto const& p : ctor_params)
            {
                std::uint8_t kind = p.kind;
                if (kind == et_valuetype || kind == et_class)
                {
                    kind = 0x12; // enum or Type — read as enum/type
                    if (p.kind == et_class)
                    {
                        kind = 0x50;
                    }
                }
                result.push_back(read_fixed_value(b, pos, kind));
            }
            return result;
        }
    };

    // Metadata model

    export struct param_info
    {
        std::string name;
        type value;
        std::uint32_t flags = 0;
        std::uint32_t sequence = 0;
    };

    export struct generic_param_info
    {
        std::string name;
        std::uint16_t number = 0;
        bool covariant = false;
        bool contravariant = false;
        // GenericParamAttributes special constraints (ECMA-335 II.23.1.7)
        bool reference_type = false;
        bool not_nullable_value_type = false;
        bool default_ctor = false;
        // explicit type constraints (GenericParamConstraint table)
        std::vector<type> constraints;
    };

    export struct method_info
    {
        std::string name;
        type ret;
        std::vector<param_info> params;
        std::vector<generic_param_info> generic_params;
        std::string overload_name;
        std::uint32_t flags = 0;
        bool is_static = false;
        bool is_virtual = false;
        bool is_abstract = false;
        bool is_special_name = false;
        bool is_default = false;
        bool is_overridable = false;
        bool is_protected = false;
        std::uint32_t method_row = 0;
    };

    export struct field_info
    {
        std::string name;
        type value;
        std::uint32_t flags = 0;
    };

    export struct property_info
    {
        std::string name;
        type value;
        std::int32_t getter = 0; // index into method list (0 = none)
        std::int32_t setter = 0;
        std::vector<param_info> index_params;
    };

    export struct event_info
    {
        std::string name;
        row type;
        std::int32_t add = 0;
        std::int32_t remove = 0;
        std::int32_t raise = 0;
    };

    export enum class type_kind
    {
        class_,
        interface,
        enum_,
        struct_,
        delegate,
    };

    export struct type_info
    {
        std::string ns;
        std::string name;
        std::uint32_t flags = 0;
        type_kind kind = type_kind::class_;
        database* db = nullptr;
        std::uint32_t row = 0;

        type_info const* base = nullptr;
        std::vector<type_info const*> interfaces;
        std::vector<type_info const*> statics;
        std::vector<type_info const*> activatable;
        bool default_activatable = false;
        type_info const* default_interface = nullptr;
        bool composable = false;
        type_info const* composable_factory = nullptr; // IFactory from [Composable(typeof(IFoo), ...)]
        std::uint32_t threading = 0;  // 1 STA, 2 MTA, 3 Both
        std::uint32_t marshaling = 0; // 1 None, 2 Agile, 3 Standard
        bool web_hidden = false;
        bool internal_ = false;
        bool deprecated = false;
        std::string deprecated_message;
        std::uint32_t version = 0;
        bool api_contract = false;
        std::uint8_t guid[16] = {};
        bool has_guid = false;

        std::vector<generic_param_info> generic_params;
        std::uint8_t enum_underlying = 0;
        std::vector<std::pair<std::string, std::int64_t>> enum_fields;
        std::vector<field_info> fields;
        std::int32_t invoke = -1;
        std::vector<method_info> methods;
        std::vector<property_info> properties;
        std::vector<event_info> events;
        // winuic-emitted attributes
        std::string winuic_json;       // WinUIAttribute — serialized view layout
        bool dark_titlebar = false;     // WinUIDarkTitlebarAttribute
        bool app_icon = false;          // WinUIAppIconAttribute
        bool dark_context_menu = false; // WinUIDarkContextMenuAttribute

        // winversec custom attributes (Win32/WDK projection)
        std::string import_library;     // DllImport/ImportLibrary attribute
        std::string dll_name;           // DllImport library name
        std::uint32_t calling_convention = 0; // CallingConvention: 0=Winapi, 1=Cdecl, 2=Stdcall, 3=Thiscall, 4=Fastcall
        std::uint32_t handle_kind = 0;  // HandleKind: 0=None, 1=File, 2=Registry, 3=Thread, etc.
        std::uint32_t buffer_semantics = 0; // BufferSemantics: 0=None, 1=Input, 2=Output, 3=InOut
        std::uint32_t string_encoding = 0; // StringEncoding: 0=None, 1=Ansi, 2=Unicode, 3=Auto
        bool enum_flags = false;        // EnumFlags attribute
        bool com_interface = false;     // ComInterface attribute
        std::uint8_t com_iid[16] = {};  // COM interface IID
        bool has_com_iid = false;
        std::uint32_t ioctl_code = 0;   // IoctlCode attribute
        bool is_inline_function = false; // InlineFunction attribute
        std::uint32_t driver_type = 0;  // DriverType: 0=None, 1=KMDF, 2=UMDF, 3=WDM
        std::uint32_t struct_layout = 0; // StructLayout: 0=None, 1=Sequential, 2=Explicit
        std::uint32_t struct_pack = 0;  // StructLayout Pack value
        bool struct_union = false;      // Union attribute
        bool last_error = false;        // LastError attribute
        bool hresult_return = false;    // HRESULT return type
        bool nt_status_return = false;  // NTStatus return type
        std::string header_source;      // Header attribute (source header file)

        std::string full_name() const
        {
            if (ns.empty()) return name;
            return ns + "." + name;
        }

        bool is_interface() const noexcept { return kind == type_kind::interface; }

        // True when the class has a real creation path: a composable factory
        // carrying a CreateInstance(...) method (plain or parameterized).
        // XAML's abstract base classes (UIElement, Panel, ...) declare empty
        // composable factories in the metadata - IUIElementFactory has zero
        // methods - so nothing can legitimately create them. Treating those
        // as non-creatable keeps the default ctor from calling factory vtable
        // slot 6 on an empty factory (reads past the vtable -> AV).
        bool factory_can_create() const noexcept
        {
            if (!composable_factory) return false;
            for (auto const& m : composable_factory->methods)
            {
                auto const& ps = m.params;
                if (ps.size() < 2) continue;
                if ((ps[ps.size() - 1].flags & param_out) == 0) continue;
                if ((ps[ps.size() - 2].flags & param_out) != 0) continue;
                if (m.ret.kind == et_void) continue;
                return true;
            }
            return false;
        }
        bool is_class() const noexcept { return kind == type_kind::class_; }
        bool is_enum() const noexcept { return kind == type_kind::enum_; }
        bool is_struct() const noexcept { return kind == type_kind::struct_; }
        bool is_delegate() const noexcept { return kind == type_kind::delegate; }
    };

    export struct namespace_info
    {
        std::vector<type_info const*> classes;
        std::vector<type_info const*> interfaces;
        std::vector<type_info const*> enums;
        std::vector<type_info const*> structs;
        std::vector<type_info const*> delegates;
    };

    export class metadata
    {
        std::vector<std::unique_ptr<database>> dbs_;
        std::vector<std::unique_ptr<type_info>> types_;
        std::map<std::string, type_info*, std::less<>> by_name_;
        std::map<std::string, type_info const*, std::less<>> by_name_c_;
        std::map<std::string, namespace_info, std::less<>> namespaces_;
        // TypeDef lookup: (db ordinal << 32 | typedef row) -> type_info
        std::unordered_map<std::uint64_t, type_info*> type_by_def_;
        std::unordered_map<database const*, std::size_t> db_ordinal_;
        // winuic: module-level attributes on <Module> TypeDef row 1
        std::string module_catalog_json_;
        bool module_extend_content_into_titlebar_ = false;
        bool module_dark_titlebar_ = false;
        bool module_dark_context_menu_ = false;
        std::string module_app_icon_path_;
        std::string module_backdrop_type_;
        std::string module_titlebar_text_;

        std::size_t db_index(database const* db) const noexcept
        {
            auto it = db_ordinal_.find(db);
            return it == db_ordinal_.end() ? std::size_t(-1) : it->second;
        }

        // resolve a TypeDefOrRef coded row in the given source database
        type_info const* resolve(row const& r, database const* source) const
        {
            if (!r.valid()) return nullptr;
            if (r.table == table_typedef)
            {
                if (!source || r.index > source->rows(table_typedef)) return nullptr;
                return lookup(source, r.index);
            }
            if (r.table == table_typeref)
            {
                if (!source || r.index > source->rows(table_typeref)) return nullptr;
                std::string ns(source->get_string(table_typeref, r.index, 2));
                std::string name(strip_arity(std::string(source->get_string(table_typeref, r.index, 1))));
                auto it = by_name_c_.find(ns + "." + name);
                if (it != by_name_c_.end()) return it->second;
                return nullptr;
            }
            if (r.table == table_typespec)
            {
                if (!source || r.index > source->rows(table_typespec)) return nullptr;
                blob b = source->get_blob(table_typespec, r.index, 0);
                type t;
                try
                {
                    t = source->parse_field_type(b);
                }
                catch (...)
                {
                    return nullptr;
                }
                if (t.kind == et_class || t.kind == et_valuetype || t.kind == et_genericinst)
                {
                    return resolve(t.type_row, source);
                }
                return nullptr;
            }
            return nullptr;
        }

        type_info const* lookup(database const* db, std::uint32_t typedef_row) const
        {
            std::size_t di = db_index(db);
            if (di == std::size_t(-1))
            {
                return nullptr;
            }
            auto it = type_by_def_.find(((std::uint64_t)di << 32) | typedef_row);
            return it == type_by_def_.end() ? nullptr : it->second;
        }

        // Section-tracking for fault diagnosis: classify_and_fill is guarded
        // wholesale by the classify pass; if a type trips a hard fault (SEH)
        // we still want to know WHICH metadata section was being read.
        static inline thread_local std::string tls_section = "entry";
        // Deliberately no restore on destruction: when an SEH fault unwinds to
        // the classify catch, tls_section must still name the failing section.
        struct section_guard
        {
            explicit section_guard(std::string s) { tls_section = std::move(s); }
        };

        void classify_and_fill(database const& db, std::uint32_t tr, type_info* t)
        {
            section_guard _sg0("extends");
            row extends = db.get_coded(table_typedef, tr, 3);
            std::string ext = db.type_ref_name(extends);

            if (t->flags & attr_interface)
            {
                t->kind = type_kind::interface;
            }
            else if (ext == "System.Enum")
            {
                t->kind = type_kind::enum_;
            }
            else if (ext == "System.ValueType")
            {
                t->kind = type_kind::struct_;
            }
            else if (ext == "System.MulticastDelegate")
            {
                t->kind = type_kind::delegate;
            }
            else
            {
                t->kind = type_kind::class_;
            }

            // generic params
            section_guard _sg1("generic-params");
            if (auto const* gps = db.gp_by_owner({ table_typedef, tr }))
            {
                for (auto const& g : *gps)
                {
                    generic_param_info gp;
                    gp.name = std::string(db.get_string(table_genericparam, g, 3));
                    gp.number = static_cast<std::uint16_t>(db.get_u32(table_genericparam, g, 0));
                    std::uint32_t gpflags = db.get_u32(table_genericparam, g, 1);
                    gp.covariant = (gpflags & 0x1) != 0;
                    gp.contravariant = (gpflags & 0x2) != 0;
                    gp.reference_type = (gpflags & 0x4) != 0;
                    gp.not_nullable_value_type = (gpflags & 0x8) != 0;
                    gp.default_ctor = (gpflags & 0x10) != 0;
                    if (auto const* cons = db.constraints_by_gp(g))
                    {
                        for (auto const& c : *cons)
                        {
                            row cr = db.get_coded(table_genericparamconstraint, c, 1);
                            // parse constraint type from its table
                            type ct;
                            ct.kind = et_end;
                            if (cr.table == table_typespec)
                            {
                                ct = db.parse_field_type(db.get_blob(table_typespec, cr.index, 0));
                            }
                            else
                            {
                                ct.kind = (cr.table == table_typedef || cr.table == table_typeref) ? et_class : et_end;
                                ct.type_row = cr;
                            }
                            gp.constraints.push_back(std::move(ct));
                        }
                    }
                    t->generic_params.push_back(std::move(gp));
                }
            }

            // methods
            section_guard _sg2("methods");
            std::uint32_t method_list = db.get_simple(table_typedef, tr, 5);
            std::uint32_t method_end = db.type_method_end(tr);
            for (std::uint32_t m = method_list; m < method_end; ++m)
            {
                method_info mi;
                mi.method_row = m;
                mi.name = std::string(db.get_string(table_methoddef, m, 3));
                if (mi.name == ".ctor" || mi.name == ".cctor")
                {
                    continue;
                }
                mi.flags = db.get_u32(table_methoddef, m, 2);
                mi.is_static = (mi.flags & method_static) != 0;
                mi.is_virtual = (mi.flags & method_virtual) != 0;
                mi.is_abstract = (mi.flags & method_abstract) != 0;
                mi.is_special_name = (mi.flags & method_special_name) != 0;

                method_sig sig = db.parse_method_signature(db.get_blob(table_methoddef, m, 4));
                mi.ret = sig.ret;                mi.params.reserve(sig.params.size());
                for (std::size_t i = 0; i < sig.params.size(); ++i)
                {
                    param_info pi;
                    pi.value = sig.params[i];
                    pi.sequence = static_cast<std::uint32_t>(i + 1);
                    mi.params.push_back(std::move(pi));
                }

                // param names from Param table
                std::uint32_t param_list = db.get_simple(table_methoddef, m, 5);
                std::uint32_t param_end = db.rows(table_param) + 1;
                if (m + 1 < method_end)
                {
                    param_end = db.get_simple(table_methoddef, m + 1, 5);
                }
                else if (m + 1 <= db.rows(table_methoddef))
                {
                    param_end = db.get_simple(table_methoddef, m + 1, 5);
                }
                for (std::uint32_t p = param_list; p < param_end && p <= db.rows(table_param); ++p)
                {
                    std::uint32_t seq = db.get_u32(table_param, p, 1);
                    if (seq == 0) continue; // return param
                    std::string pname(db.get_string(table_param, p, 2));
                    std::uint32_t pflags = db.get_u32(table_param, p, 0);
                    for (auto& pi : mi.params)
                    {
                        if (pi.sequence == seq)
                        {
                            pi.name = pname;
                            pi.flags = pflags;
                            break;
                        }
                    }
                }

                // method generic params
                if (auto const* gps = db.gp_by_owner({ table_methoddef, m }))
                {
                    for (auto const& g : *gps)
                    {
                        generic_param_info gp;
                        gp.name = std::string(db.get_string(table_genericparam, g, 3));
                        gp.number = static_cast<std::uint16_t>(db.get_u32(table_genericparam, g, 0));
                        std::uint32_t gpflags = db.get_u32(table_genericparam, g, 1);
                        gp.covariant = (gpflags & 0x1) != 0;
                        gp.contravariant = (gpflags & 0x2) != 0;
                        gp.reference_type = (gpflags & 0x4) != 0;
                        gp.not_nullable_value_type = (gpflags & 0x8) != 0;
                        gp.default_ctor = (gpflags & 0x10) != 0;
                        mi.generic_params.push_back(std::move(gp));
                    }
                }

                if (mi.name == "Invoke" && t->kind == type_kind::delegate)
                {
                    t->invoke = static_cast<std::int32_t>(t->methods.size());
                }
                else if (t->kind == type_kind::delegate && t->invoke < 0 && mi.name == t->name)
                {
                    // Non-ECMA fallback: metadata that names the delegate's
                    // method after the typedef instead of "Invoke" (older
                    // winversec emit). Keeps fnptr-typedef returns usable.
                    t->invoke = static_cast<std::int32_t>(t->methods.size());
                }

                t->methods.push_back(std::move(mi));
            }

            // fields
            section_guard _sg3("fields");
            std::uint32_t field_list = db.get_simple(table_typedef, tr, 4);
            std::uint32_t field_end = db.type_field_end(tr);
            for (std::uint32_t f = field_list; f < field_end; ++f)
            {
                field_info fi;
                fi.name = std::string(db.get_string(table_field, f, 1));
                fi.flags = db.get_u32(table_field, f, 0);
                try
                {
                    fi.value = db.parse_field_type(db.get_blob(table_field, f, 2));
                }
                catch (...)
                {
                    fi.value.kind = et_end;
                }

                if (t->kind == type_kind::enum_ && fi.name == "value__")
                {
                    t->enum_underlying = fi.value.kind;
                }
                else if (t->kind == type_kind::enum_ && (fi.flags & field_literal) && !fi.name.empty())
                {
                    // value from Constant table
                    std::int64_t value = 0;
                    if (auto const* cs = db.constants_by_field(f))
                    {
                        for (auto const& c : *cs)
                        {
                            blob vb = db.get_blob(table_constant, c, 2);
                            if (vb.size() >= 1)
                            {
                                if (vb.size() >= 4)
                                {
                                    value = (std::int64_t)read_u32_raw_in(vb);
                                }
                                else
                                {
                                    value = std::to_integer<std::uint8_t>(vb[0]);
                                }
                            }
                            break;
                        }
                    }
                    t->enum_fields.emplace_back(fi.name, value);
                }
                else
                {
                    t->fields.push_back(std::move(fi));
                }
            }

            // interface implementations
            section_guard _sg4("ifaceimpl");
            if (auto const* ii = db.iimpl_by_type(tr))
            {
                for (auto const& i : *ii)
                {
                    row ir = db.get_coded(table_interfaceimpl, i, 1);
                    type_info const* it = resolve(ir, &db);
                    if (it)
                    {
t->interfaces.push_back(it);
                    // DefaultAttribute on the InterfaceImpl row
                        for (auto const& [ca_type, value] : db.custom_attributes({ table_interfaceimpl, i }))
                        {
                            std::string name = db.attribute_type_name(ca_type);
                            if (name == "Windows.Foundation.Metadata.DefaultAttribute")
                            {
                                t->default_interface = it;
                            }
                        }
                    }
                }
            }

            // events via EventMap + MethodSemantics
            section_guard _sg5("events");
            std::uint32_t em = db.eventmap_row(tr);
            if (em != 0)
            {
                std::uint32_t ev_list = db.get_simple(table_eventmap, em, 1);
                std::uint32_t ev_end = (em < db.rows(table_eventmap)) ? db.get_simple(table_eventmap, em + 1, 1) : db.rows(table_event) + 1;
                for (std::uint32_t e = ev_list; e < ev_end; ++e)
                {
                    event_info ei;
                    ei.name = std::string(db.get_string(table_event, e, 1));
                    ei.type = db.get_coded(table_event, e, 2);
                    // method semantics
                    if (auto const* sems = db.semantics_by_assoc({ table_event, e }))
                    {
                        for (auto const& [semantics, method] : *sems)
                        {
                            if (semantics & 0x1) ei.add = (std::int32_t)method;
                            if (semantics & 0x2) ei.remove = (std::int32_t)method;
                            if (semantics & 0x4) ei.raise = (std::int32_t)method;
                        }
                    }
                    t->events.push_back(std::move(ei));
                }
            }

            // properties via PropertyMap + MethodSemantics
            section_guard _sg6("properties");
            std::uint32_t pm = db.propertymap_row(tr);
            if (pm != 0)
            {
                std::uint32_t pr_list = db.get_simple(table_propertymap, pm, 1);
                std::uint32_t pr_end = (pm < db.rows(table_propertymap)) ? db.get_simple(table_propertymap, pm + 1, 1) : db.rows(table_property) + 1;
                for (std::uint32_t p = pr_list; p < pr_end; ++p)
                {
                    property_info pi;
                    pi.name = std::string(db.get_string(table_property, p, 1));
                    try
                    {
                        pi.value = db.parse_property_type(db.get_blob(table_property, p, 2));
                    }
                    catch (...)
                    {
                        pi.value.kind = et_end;
                    }
                    // property signature also carries index params
                    try
                    {
                        method_sig psig = db.parse_property_signature(db.get_blob(table_property, p, 2));
                        for (std::size_t i = 0; i < psig.params.size(); ++i)
                        {
                            param_info pii;
                            pii.value = psig.params[i];
                            pii.sequence = static_cast<std::uint32_t>(i);
                            pi.index_params.push_back(std::move(pii));
                        }
                    }
                    catch (...)
                    {
                    }
                    if (auto const* sems = db.semantics_by_assoc({ table_property, p }))
                    {
                        for (auto const& [semantics, method] : *sems)
                        {
                            if (semantics & 0x1) pi.getter = (std::int32_t)method;
                            if (semantics & 0x2) pi.setter = (std::int32_t)method;
                        }
                    }
                    t->properties.push_back(std::move(pi));
                }
            }

            // WinRT attributes
            section_guard _sg7("typedef-attrs");

            auto attrs = db.custom_attributes({ table_typedef, tr });
            for (auto const& [ca_type, value] : attrs)
            {
                std::string name = db.attribute_type_name(ca_type);
                if (name == "Windows.Foundation.Metadata.ThreadingAttribute")
                {
                    auto args = db.parse_fixed_args(value, ctor_params(db, ca_type));
                    if (!args.empty()) t->threading = (std::uint32_t)args[0].i;
                }
                else if (name == "Windows.Foundation.Metadata.MarshalingBehaviorAttribute")
                {
                    auto args = db.parse_fixed_args(value, ctor_params(db, ca_type));
                    if (!args.empty()) t->marshaling = (std::uint32_t)args[0].i;
                }
                else if (name == "Windows.Foundation.Metadata.ActivatableAttribute")
                {
                    auto args = db.parse_fixed_args(value, ctor_params(db, ca_type));
                    if (args.empty())
                    {
                        // bare [Activatable] -> default form
                        t->default_activatable = true;
                    }
                    else if (args[0].kind == 0x50) // System.Type first arg
                    {
                        // Could be the factory form [Activatable(typeof(IFoo), ...)]
                        // or the contract-gated default form
                        // [Activatable(typeof(Contract), version)]. Resolve the
                        // type: an api_contract means the default form; anything
                        // else is a factory interface.
                        type_info const* f = find(args[0].s);
                        if (f && f->api_contract)
                        {
                            t->default_activatable = true;
                        }
                        else if (f)
                        {
                            t->activatable.push_back(f);
                        }
                        else
                        {
                            // unresolved type reference: assume factory form
                            // referenced a type outside this database
                            t->default_activatable = true;
                        }
                    }
                    else
                    {
                        // [Activatable(version)] -> default form
                        t->default_activatable = true;
                    }
                }
                else if (name == "Windows.Foundation.Metadata.StaticAttribute")
                {
                    auto args = db.parse_fixed_args(value, ctor_params(db, ca_type));
                    if (!args.empty())
                    {
                        type_info const* f = find(args[0].s);
                        if (f) t->statics.push_back(f);
                    }
                }
                else if (name == "Windows.Foundation.Metadata.ComposableAttribute")
                {
                    // [Composable(typeof(IFactory), ComposableType, contract?, version?)]
                    t->composable = true;
                    auto args = db.parse_fixed_args(value, ctor_params(db, ca_type));
                    if (!args.empty() && args[0].kind == 0x50)
                    {
                        t->composable_factory = find(args[0].s);
                    }
                }
                else if (name == "Windows.Foundation.Metadata.CompositableAttribute")
                {
                    // historical misspelling seen in some metadata; treat as
                    // composable without a known factory
                    t->composable = true;
                }
                else if (name == "Windows.Foundation.Metadata.DeprecatedAttribute")
                {
                    t->deprecated = true;
                    auto args = db.parse_fixed_args(value, ctor_params(db, ca_type));
                    if (!args.empty()) t->deprecated_message = args[0].s;
                }
                else if (name == "Windows.Foundation.Metadata.WebHostHiddenAttribute")
                {
                    t->web_hidden = true;
                }
                else if (name == "Windows.Foundation.Metadata.InternalAttribute")
                {
                    t->internal_ = true;
                }
                else if (name == "Windows.Foundation.Metadata.VersionAttribute")
                {
                    auto args = db.parse_fixed_args(value, ctor_params(db, ca_type));
                    if (!args.empty()) t->version = (std::uint32_t)args[0].i;
                }
                else if (name == "Windows.Foundation.Metadata.ApiContractAttribute")
                {
                    t->api_contract = true;
                }
                else if (name.find("WinUIAttribute") != std::string::npos && name.find("Catalog") == std::string::npos)
                {
                    // winuic: view layout JSON (string arg to WinUIAttribute..ctor(string))
                    auto args = db.parse_fixed_args(value, ctor_params(db, ca_type));
                    if (!args.empty() && !args[0].s.empty())
                    {
                        t->winuic_json = std::move(args[0].s);
                    }
                }
                else if (name.find("WinUIDarkTitlebar") != std::string::npos)
                {
                    t->dark_titlebar = true;
                }
                else if (name.find("WinUIAppIcon") != std::string::npos)
                {
                    t->app_icon = true;
                }
                else if (name.find("WinUIDarkContextMenu") != std::string::npos)
                {
                    t->dark_context_menu = true;
                }
                else if (name == "Windows.Foundation.Metadata.GuidAttribute")
                {
                    auto args = db.parse_fixed_args(value, ctor_params(db, ca_type));
                    std::uint8_t* g = t->guid;
                    // arg order: a(u32), b(u16), c(u16), d..k(u8 x8) — the
                    // remaining fixed args are the 8 Data4 bytes verbatim
                    if (args.size() >= 1) { std::uint32_t v = (std::uint32_t)args[0].i; g[0] = v & 0xFF; g[1] = (v >> 8) & 0xFF; g[2] = (v >> 16) & 0xFF; g[3] = (v >> 24) & 0xFF; }
                    if (args.size() >= 2) { std::uint16_t v = (std::uint16_t)args[1].i; g[4] = v & 0xFF; g[5] = (v >> 8) & 0xFF; }
                    if (args.size() >= 3) { std::uint16_t v = (std::uint16_t)args[2].i; g[6] = v & 0xFF; g[7] = (v >> 8) & 0xFF; }
                    for (std::size_t i = 3; i < args.size() && i < 11; ++i)
                    {
                        g[8 + i - 3] = (std::uint8_t)args[i].i;
                    }
                    t->has_guid = true;
                }
                // winversec custom attributes (Win32/WDK projection)
                else if (name.find("Winverse") != std::string::npos || name.find("Windows.Win32") != std::string::npos)
                {
                    auto args = db.parse_fixed_args(value, ctor_params(db, ca_type));
                    if (name.find("DllImportAttribute") != std::string::npos || name.find("ImportLibrary") != std::string::npos)
                    {
                        if (!args.empty() && !args[0].s.empty())
                            t->import_library = std::move(args[0].s);
                    }
                    else if (name.find("CallingConvention") != std::string::npos)
                    {
                        if (!args.empty()) t->calling_convention = (std::uint32_t)args[0].i;
                    }
                    else if (name.find("HandleKind") != std::string::npos)
                    {
                        if (!args.empty()) t->handle_kind = (std::uint32_t)args[0].i;
                    }
                    else if (name.find("BufferSemantics") != std::string::npos)
                    {
                        if (!args.empty()) t->buffer_semantics = (std::uint32_t)args[0].i;
                    }
                    else if (name.find("StringEncoding") != std::string::npos)
                    {
                        if (!args.empty()) t->string_encoding = (std::uint32_t)args[0].i;
                    }
                    else if (name.find("EnumFlags") != std::string::npos)
                    {
                        t->enum_flags = true;
                    }
                    else if (name.find("ComInterface") != std::string::npos)
                    {
                        t->com_interface = true;
                        // Parse IID from args if available
                        if (args.size() >= 1)
                        {
                            // IID is typically the first arg as a Guid-like struct
                            // For now, mark as COM interface
                            t->has_com_iid = true;
                        }
                    }
                    else if (name.find("IoctlCode") != std::string::npos)
                    {
                        if (!args.empty()) t->ioctl_code = (std::uint32_t)args[0].i;
                    }
                    else if (name.find("InlineFunction") != std::string::npos)
                    {
                        t->is_inline_function = true;
                    }
                    else if (name.find("DriverType") != std::string::npos)
                    {
                        if (!args.empty()) t->driver_type = (std::uint32_t)args[0].i;
                    }
                    else if (name.find("StructLayout") != std::string::npos)
                    {
                        if (args.size() >= 1) t->struct_layout = (std::uint32_t)args[0].i;
                        if (args.size() >= 2) t->struct_pack = (std::uint32_t)args[1].i;
                    }
                    else if (name.find("Union") != std::string::npos)
                    {
                        t->struct_union = true;
                    }
                    else if (name.find("LastError") != std::string::npos)
                    {
                        t->last_error = true;
                    }
                    else if (name.find("Header") != std::string::npos)
                    {
                        if (!args.empty() && !args[0].s.empty())
                            t->header_source = std::move(args[0].s);
                    }
                }
            }

            // method-level attributes (default/overridable/protected/overload)
            section_guard _sg8("method-attrs");
            for (auto& mi : t->methods)
            {
                auto mattrs = db.custom_attributes({ table_methoddef, mi.method_row });
                for (auto const& [ca_type, value] : mattrs)
                {
                    std::string name = db.attribute_type_name(ca_type);
                    if (name == "Windows.Foundation.Metadata.DefaultOverloadAttribute")
                    {
                        mi.is_default = true;
                    }
                    else if (name == "Windows.Foundation.Metadata.OverridableAttribute")
                    {
                        mi.is_overridable = true;
                    }
                    else if (name == "Windows.Foundation.Metadata.ProtectedAttribute")
                    {
                        mi.is_protected = true;
                    }
                    else if (name == "Windows.Foundation.Metadata.OverloadAttribute")
                    {
                        auto args = db.parse_fixed_args(value, ctor_params(db, ca_type));
                        if (!args.empty()) mi.overload_name = args[0].s;
                    }
                }
            }

            // generic param variance attributes
            section_guard _sg9("gp-variance");
            for (auto& gp : t->generic_params)
            {
                // attribute parent is the GenericParam row
                for (std::uint32_t g = 1; g <= db.rows(table_genericparam); ++g)
                {
                    row owner = db.get_coded(table_genericparam, g, 2);
                    if (!(owner.table == table_typedef && owner.index == tr)) continue;
                    std::string gname(db.get_string(table_genericparam, g, 3));
                    if (gname != gp.name) continue;
                    auto gattrs = db.custom_attributes({ table_genericparam, g });
                    for (auto const& [ca_type, value] : gattrs)
                    {
                        std::string name = db.attribute_type_name(ca_type);
                        if (name == "Windows.Foundation.Metadata.VariantAttribute") gp.covariant = true;
                        else if (name == "Windows.Foundation.Metadata.ContravariantAttribute") gp.contravariant = true;
                    }
                    break;
                }
            }

            if (t->interfaces.empty() && (t->kind == type_kind::class_))
            {
                // classes usually have at least one interface; leave as-is
            }
        }

        // ctor parameter types of an attribute's ctor (CustomAttributeType row)
        static std::vector<type> ctor_params(database const& db, row const& ca_type)
        {
            std::vector<type> params;
            if (ca_type.table == table_methoddef)
            {
                blob sig = db.get_blob(table_methoddef, ca_type.index, 4);
                try
                {
                    method_sig s = db.parse_method_signature(sig);
                    params = std::move(s.params);
                }
                catch (...)
                {
                }
            }
            else if (ca_type.table == table_memberref)
            {
                blob sig = db.get_blob(table_memberref, ca_type.index, 2);
                try
                {
                    method_sig s = db.parse_method_signature(sig);
                    params = std::move(s.params);
                }
                catch (...)
                {
                }
            }
            return params;
        }

        static std::uint32_t read_u32_raw_in(blob b)
        {
            if (b.size() < 4) return 0;
            return (std::uint32_t)(std::to_integer<std::uint8_t>(b[0])) |
                   ((std::uint32_t)(std::to_integer<std::uint8_t>(b[1])) << 8) |
                   ((std::uint32_t)(std::to_integer<std::uint8_t>(b[2])) << 16) |
                   ((std::uint32_t)(std::to_integer<std::uint8_t>(b[3])) << 24);
        }

        type_info const* find(std::string const& full_name) const
        {
            auto it = by_name_c_.find(full_name);
            return it == by_name_c_.end() ? nullptr : it->second;
        }

    public:
        metadata(std::vector<std::filesystem::path> const& files)
        {
            // Parse every input winmd in parallel: each database is fully
            // independent.  dbs_ order is preserved (db_ordinal_ and pass-1
            // duplicate resolution depend on it).
            {
                std::size_t ncores = std::max<std::size_t>(2, std::thread::hardware_concurrency());
                if (files.size() > 1 && ncores > 1)
                {
                    dbs_.resize(files.size());
                    std::atomic<std::size_t> fi{ 0 };
                    std::exception_ptr first_error;
                    std::mutex err_mutex;
                    std::vector<std::jthread> ws;
                    std::size_t nw = std::min(ncores, files.size());
                    ws.reserve(nw);
                    for (std::size_t t = 0; t < nw; ++t)
                    {
                        ws.emplace_back([&]()
                        {
                            while (true)
                            {
                                std::size_t i = fi.fetch_add(1);
                                if (i >= files.size()) break;
                                try
                                {
                                    dbs_[i] = std::make_unique<database>(files[i]);
                                }
                                catch (...)
                                {
                                    std::exception_ptr e = std::current_exception();
                                    {
                                        std::lock_guard<std::mutex> lock(err_mutex);
                                        if (!first_error) first_error = e;
                                    }
                                    break;
                                }
                            }
                        });
                    }
                    for (auto& w : ws) if (w.joinable()) w.join();
                    if (first_error) std::rethrow_exception(first_error);
                }
                else
                {
                    std::fprintf(stderr, "TRACE: loading %zu databases\n", files.size());
                    for (auto const& f : files)
                    {
                        std::fprintf(stderr, "TRACE: db load %s\n", f.string().c_str());
                        dbs_.push_back(std::make_unique<database>(f));
                        std::fprintf(stderr, "TRACE: db loaded %s\n", f.string().c_str());
                    }
                }
            }
            for (std::size_t di = 0; di < dbs_.size(); ++di)
            {
                db_ordinal_[dbs_[di].get()] = di;
            }

            // Read module-level custom attributes (WinUIControlCatalogAttribute on <Module>)
            read_module_attributes();

            // pass 1: create type_info for every WindowsRuntime TypeDef
            // Also load Win32/WDK types (non-WindowsRuntime) from winversec .winmd
            // Per-database creation is embarrassingly parallel; the name/def
            // maps are merged serially afterwards in database order, so the
            // duplicate-resolution result is identical to a serial build.
            struct p1_entry
            {
                std::uint32_t row = 0;
                std::string full_name;
                std::unique_ptr<type_info> t;
            };
            std::vector<std::vector<p1_entry>> per_db(dbs_.size());
            {
                bool const trace_p1 = std::getenv("CPPWINC_TRACE") != nullptr;
                auto p1_db = [&](std::size_t di)
                {
                    auto const& db = dbs_[di];
                    std::uint32_t n = db->rows(table_typedef);
                    if (trace_p1) std::fprintf(stderr, "TRACE: p1 db=%zu rows=%u\n", di, n);
                    auto& out = per_db[di];
                    out.reserve(n);
                    for (std::uint32_t r = 1; r <= n; ++r)
                    {
                        std::uint32_t flags = db->get_u32(table_typedef, r, 0);
                        bool is_winrt = (flags & attr_windows_runtime) != 0;
                        auto t = std::make_unique<type_info>();
                        t->db = db.get();
                        t->row = r;
                        t->flags = flags;
                        t->ns = std::string(db->get_string(table_typedef, r, 2));
                        // Normalize double-dot namespace patterns (e.g. "Windows.Win32..DBDAO")
                        {
                            auto& ns = t->ns;
                            while (ns.find("..") != std::string::npos)
                                ns.erase(ns.find("..") + 1, 1);
                        }
                        t->name = strip_arity(std::string(db->get_string(table_typedef, r, 1)));

                        // Load WindowsRuntime types AND Win32/WDK types
                        // Win32/WDK types are identified by namespace prefix
                        bool is_win32_wdk = t->ns.starts_with("Windows.Win32") || t->ns.starts_with("Windows.Wdk");
                        if (!is_winrt && !is_win32_wdk) continue;

                        out.push_back(p1_entry{ r, t->full_name(), std::move(t) });
                    }
                    if (trace_p1) std::fprintf(stderr, "TRACE: p1 db=%zu done kept=%zu\n", di, out.size());
                };
                std::size_t ncores = std::max<std::size_t>(2, std::thread::hardware_concurrency());
                if (dbs_.size() > 1 && ncores > 1)
                {
                    std::atomic<std::size_t> di_next{ 0 };
                    std::vector<std::jthread> ws;
                    std::size_t nw = std::min(ncores, dbs_.size());
                    ws.reserve(nw);
                    for (std::size_t t = 0; t < nw; ++t)
                    {
                        ws.emplace_back([&]()
                        {
                            while (true)
                            {
                                std::size_t i = di_next.fetch_add(1);
                                if (i >= dbs_.size()) break;
                                p1_db(i);
                            }
                        });
                    }
                    for (auto& w : ws) if (w.joinable()) w.join();
                }
                else
                {
                    for (std::size_t di = 0; di < dbs_.size(); ++di) p1_db(di);
                }
                // Merge in database order: identical duplicate semantics to
                // the original serial loop (first database wins).
                for (std::size_t di = 0; di < per_db.size(); ++di)
                {
                    for (auto& e : per_db[di])
                    {
                        auto [it, inserted] = by_name_.emplace(e.full_name, e.t.get());
                        if (!inserted)
                        {
                            type_by_def_.emplace(((std::uint64_t)di << 32) | e.row, it->second);
                            continue;
                        }
                        type_by_def_.emplace(((std::uint64_t)di << 32) | e.row, e.t.get());
                        types_.push_back(std::move(e.t));
                    }
                }
            }

            if (std::getenv("CPPWINC_TRACE")) std::fprintf(stderr, "TRACE: p1 merged types=%zu\n", types_.size());

            for (auto& [k, v] : by_name_)
            {
                by_name_c_.emplace(k, v);
            }

            // pass 2: classify + fill (types are independent; parallel by default)
            auto classify_range = [&](std::size_t begin, std::size_t end)
            {
                bool const trace = std::getenv("CPPWINC_TRACE") != nullptr;
                for (std::size_t i = begin; i < end; ++i)
                {
                    auto const& t = types_[i];
                    if (trace) std::fprintf(stderr, "TRACE: classify %zu/%zu %s\n", i, types_.size(), t->full_name().c_str());
                    try
                    {
                        classify_and_fill(*t->db, t->row, t.get());
                    }
                    catch (error const& e)
                    {
                        // Skip types with bad signatures (e.g. C keywords like "unsigned")
                        // instead of crashing the entire compilation
                        std::fprintf(stderr, "cppwinc : warning: skipping %s: %s\n",
                            t->full_name().c_str(), e.what());
                    }
                    catch (std::exception const& e)
                    {
                        // Real C++ exceptions from malformed metadata
                        std::fprintf(stderr, "cppwinc : warning: skipping %s: %s\n",
                            t->full_name().c_str(), e.what());
                    }
                    catch (...)
                    {
                        // Skip types whose metadata trips any other failure
                        // (bad blobs, malformed signatures) — never abort the run
                        std::fprintf(stderr, "cppwinc : warning: skipping %s (unknown failure in %s)\n",
                            t->full_name().c_str(), tls_section.c_str());
                    }
                }
            };
            if (types_.size() > 64)
            {
                std::size_t cores = std::max<std::size_t>(2, std::thread::hardware_concurrency());
                std::size_t chunk = (types_.size() + cores - 1) / cores;
                std::vector<std::future<void>> futs;
                for (std::size_t b = 0; b < types_.size(); b += chunk)
                {
                    futs.push_back(std::async(std::launch::async, classify_range, b, std::min(b + chunk, types_.size())));
                }
                for (auto& f : futs)
                {
                    f.get();
                }
            }
            else
            {
                classify_range(0, types_.size());
            }

            std::printf("-- winmd metadata: dbs=%zu pass1/classify complete, resolving bases\n", dbs_.size());

            // pass 3: resolve base types (classes and interfaces; interface
            // bases are required for correct vtable slot math in projections)
            for (auto const& t : types_)
            {
                if (t->kind != type_kind::class_ && t->kind != type_kind::interface) continue;
                row ext = t->db->get_coded(table_typedef, t->row, 3);
                if (ext.valid())
                {
                    t->base = resolve(ext, t->db);
                }
            }

            // pass 4: namespace groups
            for (auto const& t : types_)
            {
                namespace_info& ni = namespaces_[t->ns];
                switch (t->kind)
                {
                case type_kind::class_: ni.classes.push_back(t.get()); break;
                case type_kind::interface: ni.interfaces.push_back(t.get()); break;
                case type_kind::enum_: ni.enums.push_back(t.get()); break;
                case type_kind::struct_: ni.structs.push_back(t.get()); break;
                case type_kind::delegate: ni.delegates.push_back(t.get()); break;
                }
            }
        }

        std::vector<database*> databases();

        std::map<std::string, namespace_info, std::less<>> const& namespaces() const
        {
            return namespaces_;
        }

        std::string const& module_catalog_json() const { return module_catalog_json_; }
        bool module_extend_content_into_titlebar() const { return module_extend_content_into_titlebar_; }
        bool module_dark_titlebar() const { return module_dark_titlebar_; }
        bool module_dark_context_menu() const { return module_dark_context_menu_; }
        std::string const& module_app_icon_path() const { return module_app_icon_path_; }
        std::string const& module_backdrop_type() const { return module_backdrop_type_; }
        std::string const& module_titlebar_text() const { return module_titlebar_text_; }

        // Read module-level custom attributes (WinUIControlCatalogAttribute etc.)
        // from the <Module> TypeDef row 1 in each loaded database.
        void read_module_attributes()
        {
            bool const trace = std::getenv("CPPWINC_TRACE") != nullptr;
            for (auto const& db : dbs_)
            {
                if (db->rows(table_typedef) < 1) continue;
                std::uint32_t flags = db->get_u32(table_typedef, 1, 0);
                if (trace) std::fprintf(stderr, "TRACE: module attrs db flags=0x%x winrt=%d ca_rows=%u typedef_rows=%u\n", flags, (flags & attr_windows_runtime) != 0, db->rows(table_customattribute), db->rows(table_typedef));
                if (!(flags & attr_windows_runtime))
                {
                    // <Module> row — read its custom attributes
                    auto attrs = db->custom_attributes({ table_typedef, 1 });
                    if (trace) std::fprintf(stderr, "TRACE: module attrs count=%zu\n", attrs.size());
                    for (auto const& [ca_type, value] : attrs)
                    {
                        std::string aname = db->attribute_type_name(ca_type);
                        if (trace) std::fprintf(stderr, "TRACE: module attr '%s' len=%zu\n", aname.c_str(), value.size());
                        if (aname.find("WinUIControlCatalog") != std::string::npos)
                        {
                            auto args = db->parse_fixed_args(value, ctor_params(*db, ca_type));
                            if (!args.empty() && !args[0].s.empty())
                                module_catalog_json_ = std::move(args[0].s);
                        }
                        else if (aname.find("WinUIExtendContentIntoTitleBar") != std::string::npos)
                        {
                            module_extend_content_into_titlebar_ = true;
                        }
                        else if (aname.find("WinUIDarkTitlebar") != std::string::npos)
                        {
                            module_dark_titlebar_ = true;
                        }
                        else if (aname.find("WinUIDarkContextMenu") != std::string::npos)
                        {
                            module_dark_context_menu_ = true;
                        }
                        else if (aname.find("WinUIAppIcon") != std::string::npos)
                        {
                            auto args = db->parse_fixed_args(value, ctor_params(*db, ca_type));
                            if (!args.empty() && !args[0].s.empty())
                                module_app_icon_path_ = std::move(args[0].s);
                        }
                        else if (aname.find("WinUIBackdrop") != std::string::npos)
                        {
                            auto args = db->parse_fixed_args(value, ctor_params(*db, ca_type));
                            if (!args.empty() && !args[0].s.empty())
                                module_backdrop_type_ = std::move(args[0].s);
                        }
                        else if (aname.find("WinUITitlebar") != std::string::npos)
                        {
                            auto args = db->parse_fixed_args(value, ctor_params(*db, ca_type));
                            if (!args.empty() && !args[0].s.empty())
                                module_titlebar_text_ = std::move(args[0].s);
                        }
                    }
                }
            }
        }

        type_info const* find(std::string_view ns, std::string_view name) const
        {
            std::string key(ns);
            key += ".";
            key += name;
            return find(key);
        }

        // resolve a TypeDefOrRef row from any loaded database
        type_info const* find(row const& r) const
        {
            if (!r.valid())
            {
                return nullptr;
            }
            for (auto const& db : dbs_)
            {
                if (type_info const* t = resolve(r, db.get()))
                {
                    return t;
                }
            }
            return nullptr;
        }

        // resolve a signature type against the database that produced it
        type_info const* find(type const& t) const
        {
            if (t.db)
            {
                return resolve(t.type_row, t.db);
            }
            return find(t.type_row);
        }

        // resolve a signature type to its namespace and name
        bool resolve_type_name(type const& t, std::string& ns, std::string& name) const
        {
            if (!t.db || !t.type_row.valid()) return false;
            if (t.type_row.table == table_typedef)
            {
                ns = std::string(t.db->get_string(table_typedef, t.type_row.index, 2));
                name = std::string(t.db->get_string(table_typedef, t.type_row.index, 1));
                return true;
            }
            if (t.type_row.table == table_typeref)
            {
                ns = std::string(t.db->get_string(table_typeref, t.type_row.index, 2));
                name = std::string(t.db->get_string(table_typeref, t.type_row.index, 1));
                return true;
            }
            return false;
        }
    };

    inline std::vector<database*> metadata::databases()
    {
        std::vector<database*> result;
        for (auto const& db : dbs_)
        {
            result.push_back(db.get());
        }
        return result;
    }
}
