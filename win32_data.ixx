// win32_data.ixx — Win32 metadata attribute handling for CppWinC.
// Provides helper functions and emitted-code templates for Win32 API projection:
// DllImport, CallingConvention, HandleKind, BufferSemantics, StringEncoding,
// EnumFlags, ComInterface, and inline function support.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;

#include <string>
#include <string_view>
#include <cstdint>

export module win32_data;

namespace cppwinc::win32
{
    // CallingConvention strings for emitted code
    export constexpr std::string_view calling_convention_winapi = "__stdcall";
    export constexpr std::string_view calling_convention_cdecl = "__cdecl";
    export constexpr std::string_view calling_convention_stdcall = "__stdcall";
    export constexpr std::string_view calling_convention_thiscall = "__thiscall";
    export constexpr std::string_view calling_convention_fastcall = "__fastcall";

    // Get calling convention string from enum value
    export inline std::string_view get_calling_convention(std::uint32_t cc)
    {
        switch (cc)
        {
        case 1: return calling_convention_cdecl;
        case 2: return calling_convention_stdcall;
        case 3: return calling_convention_thiscall;
        case 4: return calling_convention_fastcall;
        default: return calling_convention_winapi;
        }
    }

    // HandleKind strings for emitted code
    export constexpr std::string_view handle_kind_none = "void*";
    export constexpr std::string_view handle_kind_file = "HANDLE";
    export constexpr std::string_view handle_kind_registry = "HKEY";
    export constexpr std::string_view handle_kind_thread = "HANDLE";
    export constexpr std::string_view handle_kind_process = "HANDLE";
    export constexpr std::string_view handle_kind_mutex = "HANDLE";
    export constexpr std::string_view handle_kind_event = "HANDLE";
    export constexpr std::string_view handle_kind_semaphore = "HANDLE";
    export constexpr std::string_view handle_kind_find = "HANDLE";
    export constexpr std::string_view handle_kind_file_mapping = "HANDLE";

    // Get handle type string from enum value
    export inline std::string_view get_handle_type(std::uint32_t kind)
    {
        switch (kind)
        {
        case 1: return handle_kind_file;
        case 2: return handle_kind_registry;
        case 3: return handle_kind_thread;
        case 4: return handle_kind_process;
        case 5: return handle_kind_mutex;
        case 6: return handle_kind_event;
        case 7: return handle_kind_semaphore;
        case 8: return handle_kind_find;
        case 9: return handle_kind_file_mapping;
        default: return handle_kind_none;
        }
    }

    // BufferSemantics strings for emitted code
    export constexpr std::string_view buffer_none = "";
    export constexpr std::string_view buffer_input = "in ";
    export constexpr std::string_view buffer_output = "out ";
    export constexpr std::string_view buffer_inout = "in_out ";

    // Get buffer semantics string from enum value
    export inline std::string_view get_buffer_semantics(std::uint32_t sem)
    {
        switch (sem)
        {
        case 1: return buffer_input;
        case 2: return buffer_output;
        case 3: return buffer_inout;
        default: return buffer_none;
        }
    }

    // StringEncoding strings for emitted code
    export constexpr std::string_view string_none = "";
    export constexpr std::string_view string_ansi = "LPCSTR";
    export constexpr std::string_view string_unicode = "LPCWSTR";
    export constexpr std::string_view string_auto = "LPTSTR";

    // Get string type from encoding enum
    export inline std::string_view get_string_type(std::uint32_t enc)
    {
        switch (enc)
        {
        case 1: return string_ansi;
        case 2: return string_unicode;
        case 3: return string_auto;
        default: return string_none;
        }
    }

    // P/Invoke marshaling helpers emitted into win32 modules
    export inline constexpr std::string_view pinvoke_helpers_ixx = R"cppwinc(
    // Win32 P/Invoke marshaling helpers
    namespace win32::impl
    {
        // Convert std::string to LPCWSTR for P/Invoke calls
        inline std::wstring to_wstring(std::string const& s)
        {
            if (s.empty()) return L"";
            int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            if (len <= 0) return L"";
            std::wstring result(len - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, result.data(), len);
            return result;
        }

        // RAII wrapper for Win32 handles
        class handle_holder
        {
            void* h_;
        public:
            explicit handle_holder(void* h = nullptr) noexcept : h_(h) {}
            ~handle_holder() { if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
            handle_holder(handle_holder&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
            handle_holder& operator=(handle_holder&& o) noexcept
            {
                if (this != &o) { if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); h_ = o.h_; o.h_ = nullptr; }
                return *this;
            }
            void* get() const noexcept { return h_; }
            void* release() noexcept { auto h = h_; h_ = nullptr; return h; }
            explicit operator bool() const noexcept { return h_ && h_ != INVALID_HANDLE_VALUE; }
        };

        // Buffer wrapper for marshaling arrays to/from P/Invoke
        template<typename T>
        class buffer_holder
        {
            T* data_;
            std::size_t size_;
        public:
            buffer_holder(T* data, std::size_t size) noexcept : data_(data), size_(size) {}
            T* data() const noexcept { return data_; }
            std::size_t size() const noexcept { return size_; }
        };
    }
    )cppwinc";

    // Win32 COM interface helpers
    export inline constexpr std::string_view com_interface_helpers_ixx = R"cppwinc(
    // Win32 COM interface helpers
    namespace win32::impl
    {
        // QueryInterface helper
        template<typename Interface>
        Interface* query_interface(IUnknown* unknown)
        {
            Interface* result = nullptr;
            if (unknown) unknown->QueryInterface(__uuidof(Interface), reinterpret_cast<void**>(&result));
            return result;
        }

        // Release helper
        template<typename Interface>
        void release_interface(Interface*& ptr)
        {
            if (ptr) { ptr->Release(); ptr = nullptr; }
        }
    }
    )cppwinc";

    // Enum flags helper
    export inline constexpr std::string_view enum_flags_helpers_ixx = R"cppwinc(
    // Enum flags operators
    template<typename E>
    E operator|(E a, E b) noexcept { return static_cast<E>(static_cast<std::underlying_type_t<E>>(a) | static_cast<std::underlying_type_t<E>>(b)); }
    template<typename E>
    E operator&(E a, E b) noexcept { return static_cast<E>(static_cast<std::underlying_type_t<E>>(a) & static_cast<std::underlying_type_t<E>>(b)); }
    template<typename E>
    E operator~(E a) noexcept { return static_cast<E>(~static_cast<std::underlying_type_t<E>>(a)); }
    template<typename E>
    E& operator|=(E& a, E b) noexcept { return a = a | b; }
    template<typename E>
    E& operator&=(E& a, E b) noexcept { return a = a & b; }
    )cppwinc";

    // Win32 inline function support
    export inline constexpr std::string_view inline_function_helpers_ixx = R"cppwinc(
    // Inline function support - no P/Invoke needed
    namespace win32::impl
    {
        // Mark functions that should be inlined
        #define CPPWINC_INLINE inline __forceinline
    }
    )cppwinc";
}
