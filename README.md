# CppWinC — Unified WinRT / Win32 / Driver Projection Compiler for C++23

> **DISCLAIMER — NOT A FORK.** `CppWinC` is an independent, original
> implementation written from scratch against the public Windows Runtime ABI
> and the ECMA-335 (CLI) metadata specification. It shares no code with
> Microsoft's `cppwinrt` or `cppwinrt.exe` — no copied headers, no copied
> generated code, no copied runtime. Any similarity in surface concepts
> (interface projections, activation factories) is inherent to the WinRT
> ABI itself. Licensed under MIT, Copyright (c) 2026 Momin Shaikh.

`CppWinC.exe` is a **unified** projection and component generator for C++23.
It reads ECMA-335 metadata (`.winmd` files) and emits modern C++23
**named modules** — no header files, no precompiled headers, no `pch.h`.

It projects three API surfaces from a single compiler:

| API Surface | Metadata Source | Namespaces | Runtime |
|---|---|---|---|
| **WinRT** | System `WinMetadata`, Windows SDK, WASDK/WinUI3 | `Windows.*` | `winrt_base.ixx` |
| **Win32** | `winversec`-generated `.winmd` | `Windows.Win32.*` | None (plain C++) |
| **Driver (WDK)** | `winversec`-generated `.winmd` | `Windows.Wdk.*` (shared utilities, RPC, string, network) | None (plain C++) |

WinRT modules import `winrt_base` for the runtime wrapper layer (`hstring`,
`com_ptr`, coroutines, etc.). Win32 and WDK modules are plain C++ with no
runtime dependency — direct `extern "C"` linkage, RAII handle wrappers, and
correct calling conventions.

## Headline features

- **Any WinRT API — past, present and future.** `cppwinc` is driven
  entirely by metadata: whatever `.winmd` files you feed it, it projects.
  The OS's own `WinMetadata` folder, any Windows SDK version, Windows App
  SDK, WinUI 3, Win2D, WebView2, or your own component's metadata — all
  work, unchanged, no tool update needed for new API releases.
- **Win32 API projection from winversec `.winmd`.** Feed it
  `Windows.Win32.winmd` (produced by [winversec-compiler](https://github.com/mominshaikhdev/winversec-compiler))
  and every Win32 function, struct, enum, constant, COM interface, callback,
  and handle type is projected as a type-safe C++23 module — with full
  SAL-derived buffer semantics, IOCTL structured expansion, import library
  attribution, and A/W suffix resolution already embedded in the metadata.
- **Driver API projection from winversec `.winmd`.** Feed it
  `Windows.Wdk.winmd` or `Windows.Win32+Wdk.winmd` and kernel-mode APIs
  (KMDF, UMDF, WDM, NDIS, storport, miniport, WDF framework) are projected
  as C++23 modules with IRP dispatch semantics, WDF object type attribution,
  and PnP/power state metadata — everything a driver developer needs for
  type-safe kernel-mode and user-mode driver authoring.
- **Unified WinRT + Win32 + WDK projection.** A single `CppWinC` invocation
  can project all three surfaces together. Applications import
  `Windows.Foundation`, `Windows.Win32.FileSystem`, and
  `Windows.Wdk.BASETSD` from the same compiler run.
- **C++23 modules only.** The runtime (`winrt_base`) and every projection
  are emitted as `.ixx` module interface units. Consumers `import` them
  directly — no headers, no IFC build ordering headaches. The standard
  library is a named module too: apps and the generated code use
  `import std;`, never STL headers.
- **Multi-core generation.** Metadata loading, classification and module
  generation always use all available CPU cores.
- **In-place regeneration.** Re-running into the same output directory
  overwrites regenerated modules — previously generated files from other
  input sets or user-authored modules are never deleted.
- **No hardcoded paths.** `-input local` locates the system metadata via the
  real Windows directory (never a hardcoded drive letter); a specific SDK
  version (`10.0.x`) resolves through the registry.
- **Automatic namespace splitting.** Large namespaces (e.g. `Microsoft.UI.Xaml`
  with 94K+ methods) are automatically split into balanced sub-modules
  (`P1`, `P2`, ...) that compile independently. Cross-chunk forward
  declarations are emitted so sibling chunks resolve type references.
- **Complete base-chain projection.** Inherited interface methods are
  projected into derived classes (up to 5000 methods per type), giving
  consumers full access to inherited properties, events and methods without
  casting.

## WinRT Projection

### Example — Async file I/O

```cpp
import std;
import winrt_base;
import winrt.Windows.Foundation;
import winrt.Windows.Storage;

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;

int main() {
    auto file = StorageFile::GetFileFromPathAsync(L"data.txt").get();
    auto stream = file.OpenReadAsync().get();
    auto reader = DataReader(stream);
    reader.LoadAsync(1024).get();
    auto text = reader.ReadString(reader.UnconsumedBufferLength());
    std::wcout << std::wstring(text) << std::endl;
}
```

### Example — WinRT collections and property sets

```cpp
import std;
import winrt_base;
import winrt.Windows.Foundation;
import winrt.Windows.Foundation.Collections;

using namespace winrt;
using namespace winrt::Windows::Foundation::Collections;

int main() {
    PropertySet props;
    props.Insert(L"Width", IInspectable(box_value(800)));
    props.Insert(L"Height", IInspectable(box_value(600)));
    props.Insert(L"Title", IInspectable(box_value(L"App")));

    for (auto const& [key, value] : props) {
        auto boxed = value.try_unbox<int32_t>();
        if (boxed)
            std::wcout << key << L" = " << *boxed << std::endl;
        else
            std::wcout << key << L" = (string)" << std::endl;
    }
}
```

### Example — WinRT coroutines

```cpp
import std;
import winrt_base;
import winrt.Windows.Foundation;

using namespace winrt;
using namespace winrt::Windows::Foundation;

IAsyncOperation<int> compute_async() {
    co_await winrt::resume_background();
    co_return 42;
}

int main() {
    auto result = compute_async().get();
    std::cout << "Result: " << result << std::endl;
}
```

## Win32 API Projection

`CppWinC` consumes `Windows.Win32.winmd` produced by
[winversec-compiler](https://github.com/mominshaikhdev/winversec-compiler)
and generates type-safe C++23 module projections for all Win32 APIs.

Win32 modules import **no runtime** — plain C++ types with `extern "C"`
linkage, `__stdcall` calling conventions, and RAII handle wrappers.

### What is projected

| Category | Examples | Module |
|---|---|---|
| **File system** | `CreateFile`, `ReadFile`, `WriteFile`, `FindFirstFile` | `Windows.Win32.Storage.FileSystem` |
| **Registry** | `RegOpenKeyEx`, `RegQueryValueEx`, `RegSetValueEx` | `Windows.Win32.System.Registry` |
| **Window management** | `CreateWindowEx`, `DefWindowProc`, `GetMessage` | `Windows.Win32.WINUSER` |
| **GDI** | `BitBlt`, `CreateSolidBrush`, `TextOut` | `Windows.Win32.Graphics.Gdi` |
| **Direct3D 11/12** | `D3D11CreateDevice`, `D3D12CreateDevice` | `Windows.Win32.Graphics.Direct3D11` |
| **COM** | `CoInitializeEx`, `CoGetMalloc`, `CreateStreamOnHGlobal` | `Windows.Win32.Com` |
| **Threading** | `CreateThread`, `WaitForSingleObject`, `EnterCriticalSection` | `Windows.Win32.System.Threading` |
| **Networking** | `WSAStartup`, `socket`, `connect`, `send`, `recv` | `Windows.Win32.Networking.WinSock` |
| **IO/IOCTL** | `DeviceIoControl`, `CreateIoCompletionPort` | `Windows.Win32.System.IO` |
| **Services** | `OpenSCManagerW`, `CreateServiceW`, `StartServiceW` | `Windows.Win32.System.Services` |
| **Shell** | `ShellExecute`, `SHGetFolderPath` | `Windows.Win32.UI.Shell` |
| **Debug** | `OutputDebugString`, `ReadProcessMemory` | `Windows.Win32.Diagnostics.Debug` |

### Projection semantics

- **Handle wrappers.** `HANDLE`, `HWND`, `HKEY`, `HMODULE`, `SC_HANDLE`,
  `SOCKET` and 20+ handle types project as typed RAII wrappers.
- **Buffer marshaling.** SAL annotations drive automatic buffer marshaling.
- **Calling convention.** `WINAPI`/`CALLBACK`/`cdecl`/`stdcall` projected
  as `__stdcall`, `__cdecl`, etc.
- **Import library.** `DllImport`/`ImportLibrary` tell the linker which
  `.lib` or `.dll` to use.
- **A/W resolution.** `CreateFileA`/`CreateFileW` resolve to `CreateFile`.
- **EnumFlags.** Bitmask enums get `|`, `&`, `~`, `|=`, `&=` operators.
- **StructLayout/Union.** Sequential, explicit, and union layouts with
  custom packing projected with correct memory layout.
- **Inline functions.** Functions marked `InlineFunction` emit inline bodies.

### Example — Win32 file I/O

```cpp
import std;
import Windows.Win32.Storage.FileSystem;

int main() {
    using namespace Windows::Win32::Storage::FileSystem;

    auto file = CreateFile(L"data.txt", GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);

    char buf[4096]{};
    DWORD bytesRead = 0;
    ReadFile(file.get(), buf, sizeof(buf), &bytesRead, nullptr);
    CloseHandle(file.get());

    std::cout << "Read " << bytesRead << " bytes" << std::endl;
}
```

### Example — Win32 registry

```cpp
import Windows.Win32.System.Registry;

int main() {
    using namespace Windows::Win32::System::Registry;

    HKEY key = nullptr;
    RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\MyApp", 0, KEY_READ, &key);

    char value[256]{};
    DWORD size = sizeof(value);
    RegQueryValueEx(key, L"InstallPath", nullptr, nullptr,
        reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
}
```

### Example — Win32 window creation

```cpp
import Windows.Win32.WINUSER;
import Windows.Win32.Graphics.Gdi;

using namespace Windows::Win32::WINUSER;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int main() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"Win32App";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, L"Win32App", L"Hello Win32",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        800, 600, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    ShowWindow(hwnd, SW_SHOW);

    MSG msg{};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
```

## Driver (WDK) API Projection

`CppWinC` consumes `Windows.Wdk.winmd` produced by
[winversec-compiler](https://github.com/mominshaikhdev/winversec-compiler)
and generates C++23 module projections for driver-related APIs.

Like Win32, WDK modules import **no runtime** — plain C++ types with
proper calling conventions and kernel-mode semantics.

### What is projected

The compiler auto-detects any namespace starting with `Windows.Wdk.*`
as WDK and projects accordingly. Module names match the namespace from the
`.winmd` (e.g. `import Windows.Wdk.BASETSD;`, `import Windows.Wdk.STRSAFE;`).

The current WDK `.winmd` covers shared utility headers — type-safe
integer conversion macros, string safety helpers, RPC/NDR bindings,
network extension helpers, and related low-level APIs. Core WDM/KMDF/UMDF
driver APIs (`IoCreateDevice`, `WdfDeviceCreate`, etc.) require
additional winversec coverage of `ntddk.h`, `wdm.h`, `fltKernel.h`,
and WDF framework headers.

### Driver projection semantics

- **Calling conventions.** WDM: `__stdcall` (NTAPI). KMDF/UMDF: default.
- **Import libraries.** `ntoskrnl.exe` for WDM, `wdfldr.dll` for KMDF,
  `wdfumdf.dll` for UMDF — or from `ImportLibrary` attribute.
- **StructLayout/Union.** Kernel structures with explicit layouts,
  unions, and custom packing are projected correctly.
- **Inline functions.** Functions marked `InlineFunction` emit inline
  bodies (e.g. `HandleToLong`, `StringCbCopy`, `RtlIpv4AddressToString`).

### Example — Type-safe integer conversions (BASETSD)

```cpp
import Windows.Wdk.BASETSD;

using namespace Windows::Wdk::BASETSD;

void* handle = /* ... */;
int32_t value = HandleToLong(handle);
void* restored = LongToHandle(value);

uintptr_t ptr = /* ... */;
int32_t as_int = PtrToInt(ptr);
void* from_int = IntToPtr(as_int);
```

### Example — String safety (STRSAFE)

```cpp
import Windows.Wdk.STRSAFE;

using namespace Windows::Wdk::STRSAFE;

char dest[256]{};
const char* src = "Hello, kernel!";
StringCbCopy(dest, sizeof(dest), src);
StringCatN(dest, sizeof(dest), " suffix", 7);
```

### Example — Network extensions (PACKET / RING)

```cpp
import Windows.Wdk.PACKET;
import Windows.Wdk.RING;

using namespace Windows::Wdk::PACKET;
using namespace Windows::Wdk::RING;

// Query packet extensions and ring buffer management
// for high-performance network driver I/O
```

### Example — RPC client (RPCDCE)

```cpp
import Windows.Wdk.RPCDCE;

using namespace Windows::Win32::RPCDCE;

RPC_BINDING_BINDING binding = nullptr;
RPC_STATUS status = RpcBindingFromStringBinding(
    L"ncalrpc:[endpoint]", &binding);
if (status == 0) {
    // Use binding...
    RpcBindingFree(&binding);
}
```

### Example — TPM base services (TBS)

```cpp
import Windows.Wdk.TBS;

using namespace Windows::Wdk::TBS;

TBS_HCONTEXT context = 0;
TBS_CONTEXT_PARAMS2 params{};
params.version = 2;
params.requestedExecution = TBS_COMMAND_LOCALITY_ZERO;
TBSi_Context_Create(&params, &context);
// Submit TPM commands...
Tbsip_Context_Close(context);
```

## Combined WinRT + Win32 + WDK

### Generate all three projections

```bat
cppwinc.exe -input local,Windows.Win32.winmd,Windows.Wdk.winmd -base -output out
```

### Example — WinRT async + Win32 file I/O

```cpp
import std;
import winrt_base;
import winrt.Windows.Foundation;
import Windows.Win32.Storage.FileSystem;

using namespace winrt;
using namespace winrt::Windows::Foundation;

IAsyncAction SaveDataAsync(std::wstring const& path, std::string const& data) {
    co_await winrt::resume_background();

    using namespace Windows::Win32::Storage::FileSystem;
    auto file = CreateFile(path.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, 0, nullptr);

    if (file.get() == INVALID_HANDLE_VALUE) co_return;

    DWORD written = 0;
    WriteFile(file.get(), data.c_str(), static_cast<DWORD>(data.size()),
        &written, nullptr);
    CloseHandle(file.get());
    co_return;
}

int main() {
    auto action = SaveDataAsync(L"output.txt", "Hello from CppWinC!");
    action.get();
}
```

### Example — Win32 + WDK shared types

```cpp
import std;
import Windows.Win32.System.Threading;
import Windows.Wdk.BASETSD;

void inspect_device_state() {
    using namespace Windows::Win32::System::Threading;
    DWORD pid = GetCurrentProcessId();
    std::cout << "PID: " << pid << std::endl;
}

// WDK type conversions are available from Windows.Wdk.BASETSD
void convert_handles() {
    using namespace Windows::Wdk::BASETSD;
    void* handle = /* ... */;
    int32_t value = HandleToLong(handle);
    void* restored = LongToHandle(value);
}
```

## Usage

```
cppwinc.exe [options...]

Options:
    -input <spec>        Windows metadata to include - ONE comma-separated list
                         (local, <path-to-winmd-or-folder>, 10.0.x); -winui
                         auto-adds WASDK metadata and finds its runtime DLLs
    -output <path>       Location of generated projection
    -component [<path>]  Generate component templates and implementation
    -name <name>         Specify explicit name for component files
    -verbose             Show detailed progress information
    -include <prefix>    One or more prefixes to include in input
    -exclude <prefix>    One or more prefixes to exclude from input
    -base                Generate base module unconditionally
    -std [<path>]        Build the std named module into <path>
    -winui               Auto-discover WASDK metadata + DLLs, build std, compile all modules
    -winui_no_compile    Like -winui but skip compilation (generate only)
    -help                Show detailed help with examples
    -module_include <ns> Only generate modules for the listed namespaces
    -module_exclude <ns> Skip modules for the listed namespaces

Where <spec> is a single comma-separated list of any of:
  path                Path to winmd file or recursively scanned folder
  local               Local %WinDir%\System32\WinMetadata folder
  10.0.12345.0        Specific version of the Windows SDK metadata
```

### Generate a projection

```bat
:: OS metadata plus a folder of extra winmd files (e.g. WASDK/WinUI3)
cppwinc.exe -input local,<path\to\winmd-folder> -base -output out

:: WinUI app - -winui finds WASDK metadata + DLLs automatically
cppwinc.exe -input local,MyApp.winmd -winui

:: Multiple winmd files, comma-separated
cppwinc.exe -input Xaml.winmd,Foundation.winmd -base -output out
```

### Generate a Win32 projection

```bat
cppwinc.exe -input Windows.Win32.winmd -base -output out
```

### Generate a Driver (WDK) projection

```bat
cppwinc.exe -input Windows.Wdk.winmd -base -output out
:: Or both Win32 + WDK together, comma-separated
cppwinc.exe -input Windows.Win32.winmd,Windows.Wdk.winmd -base -output out
```

### Build the std module

```bat
:: one-time per toolchain — builds std.ifc + std.obj
cppwinc.exe -std
```

### Compile generated modules

```bat
set BUILD_DIR=build
cl /nologo /std:c++latest /EHsc /MD /O2 /DNDEBUG /FS /utf-8 /bigobj ^
    /ifcSearchDir %BUILD_DIR% /ifcOutput %BUILD_DIR% /Fo%BUILD_DIR%\ /c ^
    out\winrt_base.ixx out\winrt.Windows.Foundation.ixx app.ixx
link /nologo /out:app.exe app.obj %BUILD_DIR%\winrt_base.obj ^
    %BUILD_DIR%\winrt.Windows.Foundation.obj std.obj windowsapp.lib
```

### One-shot build with `-winui`

The `-winui` flag handles everything in one command — auto-discovers
WASDK metadata and runtime DLLs from NuGet, builds the `std` module,
generates the projection, and compiles all modules in topological order:

```bat
cppwinc.exe -input MyApp.winmd -winui -output .
```

This automatically:
1. Finds the latest WASDK NuGet packages (`microsoft.windowsappsdk.foundation`,
   `microsoft.windowsappsdk.winui`) and adds their metadata as inputs.
2. Locates the latest WinUI 3 runtime DLLs for framework-dependent apps.
3. Builds `std.ifc` using the detected MSVC toolchain (auto if missing).
4. Generates `winrt_base.ixx` (auto, no `-base` flag needed).
5. Generates component modules with full WinRT activation structs
   (`create_instance`, `iface_cache`, `implements_type`, `default_interface`).
6. Generates view modules from `.winui` data (App, MainWindow, pages).
7. Emits `main.ixx` with WASDK bootstrap (`MddBootstrapInitialize2`).
8. Generates `.ixx` modules, splitting large namespaces (e.g. `Microsoft.UI.Xaml`
   with 94K+ methods) into balanced chunks (`P1`, `P2`, ...).
9. Compiles all modules using direct `cl.exe` invocation with auto-detected
   MSVC toolchain, SDK, and WASDK include paths.

No hardcoded paths — works on any Windows machine with Visual Studio and NuGet.

#### Component module structs

The component module (`<Name>.ixx`) exports a struct per WinRT runtime class:

```cpp
export struct MainWindow {
    static constexpr std::wstring_view class_name() noexcept { return L"App.MainWindow"; }
    winrt::com_ptr<void> m_unknown;
    mutable winrt::impl::iface_cache m_cache;
    static MainWindow from_abi(void* ptr) noexcept;
    template <typename I> void* iface() const;
    operator winrt::IInspectable() const noexcept;
    MainWindow() : m_unknown(winrt::impl::create_instance(class_name())) {}
    using implements_type = MainWindow;
    using default_interface = winrt::IInspectable;
};
```

These structs wrap WinRT activation — constructing one creates the runtime
instance via the activation factory. The App module uses `Window::from_abi()`
to project the instance and call `Activate()`.

#### Brush and geometry property handling

The codegen automatically handles type-incompatible properties:
- `Foreground`/`Background`/`IconSource` (Brush types) — skipped when value is
  empty or a `{ThemeResource}` XAML resource reference.
- `Data` (Geometry type) — emits `Geometry()` placeholder.
- `FontFamily` — emits `FontFamily()` default.
- Hex color values (#RGB, #RRGGBB) — wrapped in `SolidColorBrush` via
  `make_solid_brush(Color{...})` with ARGB conversion.
- Untyped item bindings (DataTemplate `item.Property`) — skipped for
  IInspectable items to avoid type mismatches.

## Module compilation

All generated modules compile with MSVC `/std:c++latest`. Key fixes:

- **Property/event wrappers** are out-of-line to avoid scope issues with
  inherited methods.
- **Base-chain method declarations** emit inherited methods from parent
  classes with up to 5000 methods per type.
- **Cross-namespace forward declarations** are emitted when modules are
  split, so P-chunks referencing types from sibling chunks compile.
- **`/FS` flag** ensures concurrent `.pdb` writes during parallel compilation.
- **Pruned projection** — only compile modules the app transitively imports,
  not the entire WinRT surface.

## Runtime helpers

Everything an app needs beyond the metadata projection lives in
`winrt_base.ixx` (`module winrt_base;`), emitted from `strings_data.ixx`:

- **`hstring`** — immutable UTF-16 string with reference counting; implicit
  conversion to/from `std::wstring`, `wchar_t const*`, and `std::string_view`.
  Construct with `L"literal"` or `hstring(std::wstring(...))`.
- **`winrt_guid`** — 128-bit GUID value type; equality comparison, hashing,
  and conversion to/from `GUID`. Used for interface IDs throughout the
  projection.
- **`com_ptr<T>`** — intrusive reference-counted smart pointer for COM
  interfaces. Calls `AddRef`/`Release` automatically. `put_abi()` returns
  `void**` for out-parameter initialization. `copy_from()` / `copy_to()`
  for safe COM pointer transfers.
- **Marshaling helpers.**
  - `arg_in<T>` — marshals an input parameter from ABI to projected type.
  - `put_abi<T>` — returns `void**` for COM out-parameters.
  - `bind_in<T>` — binds an input buffer for marshaling.
  - `detach_abi<T>` — detaches ownership from a `com_ptr` to raw ABI pointer.
  - `attach_abi<T>` — attaches a raw ABI pointer to a `com_ptr`.
- **Delegates.** Generated callable constructors for WinRT delegate types.
  A delegate is a `com_ptr` wrapping a callback; the projection generates
  a `call()` method and comparison operators.
- **Coroutines.**
  - `IAsyncAction` — coroutine type for async operations with no return value.
  - `IAsyncOperation<T>` — coroutine type for async operations returning `T`.
  - `IAsyncActionWithProgress<P>` / `IAsyncOperationWithProgress<T,P>` —
    progress-reporting variants.
  - `resume_background()` — resumes coroutine on a thread-pool thread.
  - ` resume_after(duration)` — resumes after a time delay.
  - ` single_threaded_observable_vector<T>()` — creates an observable vector.
  - Full `co_await` support: `auto x = co_await async_op;` works seamlessly.
- **Boxing / unboxing.**
  - `box_value(T)` — boxes a value type into `IInspectable`.
  - `unbox_value<T>(IInspectable)` — unboxes back to `T`.
  - `try_unbox<T>(IInspectable)` — returns `std::optional<T>`, empty if
    type mismatch.
  - Boxed types: `int32_t`, `uint32_t`, `int64_t`, `float`, `double`,
    `bool`, `hstring`, `guid`, `datetime`, `time_span`, `point`, `size`,
    `rect`, and any WinRT type.
- **Errors.**
  - `hresult_error` — exception type wrapping an `HRESULT` error code.
  - `check_hresult(HRESULT)` — throws `hresult_error` if failed.
  - `throw_hresult(HRESULT)` — always throws.
  - `winrt::hresult_access_denied`, `hresult_invalid_argument`, etc. —
    typed exception classes for common error codes.
- **Collections.**
  - `single_threaded_vector<T>()` — creates `IVector<T>`.
  - `single_threaded_observable_vector<T>()` — creates
    `IObservableVector<T>`.
  - `single_threaded_map<K,V>()` — creates `IMap<K,V>`.
  - `single_threaded_threadsafe_map<K,V>()` — thread-safe variant.
  - `single_threaded_vector_view<T>(vector<T>)` — creates `IVectorView<T>`.
  - `single_threaded_map_view<K,V>(map)` — creates `IMapView<K,V>`.
- **Cancellable coroutines** — full `IAsyncAction`/`IAsyncOperation<T>`
  implementations with cancellation token support. The runtime manages
  the coroutine state machine, reference counting, and ABI vtable layout.
- **Component-authoring helpers.**
  - `implements<D, I...>` — CRTP base for implementing WinRT interfaces.
  - `interface_impl_base<I>` — base for interface implementation.
  - `make<T>()` / `make_self<T>()` / `get_self<T>()` — allocation helpers.
  - Activation factories — `create_instance(class_name)` creates a WinRT
    runtime class instance via its activation factory.
  - Event token management — `event_token` registration and revocation.
  - Threading — `apartment_context` for capturing/resuming on the UI thread.
- **String helpers.**
  - `to_hstring(std::string)` — converts narrow string to `hstring`.
  - `to_hstring(std::wstring)` — converts wide string to `hstring`.
  - `to_string(hstring)` — converts `hstring` to `std::string`.
  - `to_wstring(hstring)` — converts `hstring` to `std::wstring`.

> **Note:** `winrt_base` is only used by WinRT projection modules.
> Win32 and WDK modules do not import `winrt_base` — they use plain C++ types.

## Long paths

`CppWinC` is long-path aware: no fixed-size path buffers, all file I/O
through `std::filesystem`, Windows directory/registry reads grow dynamically.

## Building the tool

```bat
build.bat
:: -> cppwinc.exe (release: /O2, /GL, /LTCG, /DNDEBUG)
```

The tool is C++23 named modules:

```
cppwinrtc-compiler/
├── cppwinc.ixx              # Module root, includes all sub-modules
├── cppwinc_core.ixx         # Core metadata classification
├── cppwinc_cli.ixx          # CLI argument parsing
├── cppwinc_types.ixx        # Type mapping, naming, signatures
├── cppwinc_proj.ixx         # Projection engine: WinRT types, ABI helpers
├── cppwinc_comp.ixx         # Component template generation
├── cppwinc_driver.ixx       # Win32 + WDK projection writers + driver logic
├── winmd_reader.ixx         # ECMA-335 metadata reader
├── async_data.ixx           # async/coroutine runtime text
├── strings_data.ixx         # emitted runtime and templates
├── component_data.ixx       # component/factory emitted text
├── winuic_data.ixx          # WinUI3 data types and helpers
├── winuic_helper.ixx        # WinUI3 helper functions
├── win32_data.ixx           # Win32 metadata attribute helpers
├── wdk_data.ixx             # WDK metadata attribute helpers
├── main.cpp                 # CLI entry
├── build.bat                # incremental release build
└── README.md
```

## Related Projects

| Project | Description |
|---|---|
| [winversec-compiler](https://github.com/mominshaikhdev/winversec-compiler) | Win32 + WDK metadata compiler — produces `.winmd` from Windows SDK C/C++ headers |
| [xaml2winuic-compiler](https://github.com/mominshaikhdev/xaml2winuic-compiler) | XAML-to-`.winui` transpiler — converts WinUI 3 XAML markup into the `.winui` DSL |
| [winuic-compiler](https://github.com/mominshaikhdev/winuic-compiler) | `.winui` DSL compiler — compiles declarative views into projection-agnostic `.winmd` metadata |

## License

MIT License — see `LICENSE`. Copyright (c) 2026 Momin Shaikh. All rights
reserved. Not affiliated with or endorsed by Microsoft.
