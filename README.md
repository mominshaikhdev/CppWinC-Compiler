# CppWinC — Unified WinUI 3 / WinRT / Win32 / Driver Projection Compiler for C++23

> **DISCLAIMER — NOT A FORK.** `CppWinC` is an independent, original
> implementation written from scratch against the public Windows Runtime ABI
> and the ECMA-335 (CLI) metadata specification. It shares no code with
> Microsoft's `cppwinrt` or `cppwinrt.exe` — no copied headers, no copied
> generated code, no copied runtime. Any similarity in surface concepts
> (interface projections, activation factories) is inherent to the WinRT
> ABI itself. Licensed under MIT, Copyright (c) 2026 Momin Shaikh.

---

## Table of Contents

1. [Headline Features](#headline-features)
2. [WinUI 3 Projection](#winui-3-projection)
   - [Manual Editing, Business Logic & Third-Party Control Integration](#manual-editing-business-logic--third-party-control-integration)
   - [Metadata-First WinUI 3 Architecture & Rebuild-Free Evolution](#metadata-first-winui-3-architecture--rebuild-free-evolution)
   - [Shared WinUI 3 Base Module (`winui_base`)](#shared-winui-3-base-module-winui_base)
   - [Brush and Color Handling](#brush-and-color-handling)
   - [Geometry and SVG Path Handling](#geometry-and-svg-path-handling)
   - [Font and Icon Handling](#font-and-icon-handling)
   - [Dynamic Resource Dictionary & Theme Lookup](#dynamic-resource-dictionary--theme-lookup)
   - [Untyped Item Property Access & DataTemplate Handling](#untyped-item-property-access--datatemplate-handling)
3. [WinRT Projection](#winrt-projection)
   - [Runtime Helpers (`winrt_base`)](#runtime-helpers-winrt_base)
   - [Component Module Structs](#component-module-structs)
   - [Example — Async File I/O](#example--async-file-io)
   - [Example — WinRT Collections and Property Sets](#example--winrt-collections-and-property-sets)
   - [Example — WinRT Coroutines](#example--winrt-coroutines)
4. [Win32 API Projection](#win32-api-projection)
   - [What is Projected](#what-is-projected)
   - [Projection Semantics](#projection-semantics)
   - [Example — Win32 File I/O](#example--win32-file-io)
   - [Example — Win32 Registry](#example--win32-registry)
   - [Example — Win32 Window Creation](#example--win32-window-creation)
5. [Driver (WDK) API Projection](#driver-wdk-api-projection)
   - [What is Projected](#what-is-projected-1)
   - [Driver Projection Semantics](#driver-projection-semantics)
   - [Example — Type-Safe Integer Conversions (BASETSD)](#example--type-safe-integer-conversions-basetsd)
   - [Example — String Safety (STRSAFE)](#example--string-safety-strsafe)
   - [Example — Network Extensions (PACKET / RING)](#example--network-extensions-packet--ring)
   - [Example — RPC Client (RPCDCE)](#example--rpc-client-rpcdce)
   - [Example — TPM Base Services (TBS)](#example--tpm-base-services-tbs)
6. [Combined WinUI 3 + WinRT + Win32 + WDK](#combined-winui-3--winrt--win32--wdk)
   - [Generate All Four Projections](#generate-all-four-projections)
   - [Example — WinRT Async + Win32 File I/O](#example--winrt-async--win32-file-io)
   - [Example — Win32 + WDK Shared Types](#example--win32--wdk-shared-types)
7. [External C++ code: headers, libs, DLLs (externals/)](#external-c-code-headers-libs-dlls-externals)
8. [Usage & CLI Reference](#usage)
   - [Generate a Projection](#generate-a-projection)
   - [Generate a Win32 Projection](#generate-a-win32-projection)
   - [Generate a Component with `-component`](#generate-a-component-with--component)
   - [Authoring a General WinRT Component (`-component` for Plain APIs)](#authoring-a-general-winrt-component--component-for-plain-apis)
   - [MIDL-Authored Components (`midlrt` + `-component`)](#midl-authored-components-midlrt---component)
   - [Generate a Driver (WDK) Projection](#generate-a-driver-wdk-projection)
   - [Build the std Module](#build-the-std-module)
   - [Compile Generated Modules](#compile-generated-modules)
   - [Build with `-compile` (and `-target`)](#build-with--compile-and--target)
   - [One-Shot Build with `-winui`](#one-shot-build-with--winui)
     - [App Naming](#app-naming)
     - [Experimental Channel (`exp`)](#experimental-channel-exp)
     - [Debug Builds (`debug`)](#debug-builds-debug)
     - [System Metadata and Module Selection](#system-metadata-and-module-selection)
9. [Module Compilation](#module-compilation)
10. [Long Paths](#long-paths)
11. [Building the Tool](#building-the-tool)
12. [Related Projects](#related-projects)
13. [License](#license)

---

`CppWinC.exe` is a **unified** projection and component generator for C++23.
It reads ECMA-335 metadata (`.winmd` files) and emits modern C++23
**named modules** — no header files, no precompiled headers, no `pch.h`.

It projects four API surfaces from a single compiler:

| API Surface | Metadata Source | Namespaces | Runtime |
|---|---|---|---|
| **WinUI 3** | `.winui` XAML views (xaml2winuic / winuic), WASDK | `Microsoft.UI.*` | `winui_base.ixx` |
| **WinRT** | System `WinMetadata`, Windows SDK, WASDK/WinUI3 | `Windows.*` | `winrt_base.ixx` |
| **Win32** | `winversec`-generated `.winmd` | `Windows.Win32.*` | None (plain C++) |
| **Driver (WDK)** | `winversec`-generated `.winmd` | `Windows.Wdk.*` (shared utilities, RPC, string, network) | None (plain C++) |

WinUI 3 modules import `winui_base` and WinRT modules import `winrt_base` for the runtime wrapper layer
(`hstring`, `com_ptr`, coroutines, etc.). Win32 and WDK modules are plain C++
with no runtime dependency — direct `extern "C"` linkage, RAII handle wrappers,
and correct calling conventions.

---

## Headline features

- **WinUI 3 / XAML projection.** Feed `.winui` XAML views (transpiled by
  [xaml2winuic-compiler](https://github.com/mominshaikhdev/xaml2winuic-compiler)
  / [winuic-compiler](https://github.com/mominshaikhdev/winuic-compiler)) or
  Windows App SDK metadata and `cppwinc` projects the WinUI 3 surface
  (`Microsoft.UI.*` / `Microsoft.UI.Xaml.*`) as C++23 modules — controls,
  layouts, resources, and typed-event handlers — importable
  alongside WinRT, Win32, and WDK from the same run (`-winui`).
- **Metadata-First in WinUI 3 & Rebuild-Free Architecture.** `cppwinc` implements a
  pure **metadata-first** architecture for WinUI 3 applications. Rather than maintaining
  hardcoded control lists or requiring compiler updates when new controls or WinUI Gallery
  releases appear, `cppwinc` dynamically inspects the ECMA-335 `.winmd` metadata (carrying
  `WinUIAttribute` view JSON and `WinUIControlCatalogAttribute`). It auto-discovers all
  referenced controls, resolves namespaces against the metadata type system, dynamically
  seeds closure roots, and dynamically instantiates external control resource dictionaries
  (such as `TabularControlsResources`, `XamlChartsResources`) at launch. Future controls and
  future WinUI Gallery releases require **zero rebuilds** of `cppwinc.exe`.
- **Any Windows APIs — past, present and future.** `cppwinc` is driven
  entirely by metadata: whatever `.winmd` files you feed it, it projects.
  The OS's own `WinMetadata` folder, any Windows SDK version, Windows App
  SDK, WinUI 3, Win2D, WebView2, or your own component's metadata — all
  work, unchanged, no tool update needed for new API releases.
- **Win32 API projection from winversec `.winmd`.** Feed it
  `Windows.Win32.winmd` (produced by [winversec-compiler](https://github.com/mominshaikhdev/winversec-compiler))
  and every Win32 function, struct, enum, constant, COM interface, callback,
  and handle type is projected as a type-safe C++23 module — with full
  SAL-derived buffer semantics, IOCTL structured expansion, import library
  attribution, and A/W suffix resolution (unsuffixed canonical forwarders
  to the W variant, emitted by `cppwinc`).
- **Driver API projection from winversec `.winmd`.** Feed it
  `Windows.Wdk.winmd` or `Windows.Win32+Wdk.winmd` and kernel-mode APIs
  (KMDF, UMDF, WDM, NDIS, storport, miniport, WDF framework) are projected
  as C++23 modules with IRP dispatch semantics, WDF object type attribution,
  and PnP/power state metadata — everything a driver developer needs for
  type-safe kernel-mode and user-mode driver authoring.
- **Unified WinUI 3 + WinRT + Win32 + WDK projection.** A single `CppWinC`
  invocation can project all four surfaces together. Applications import
  `Microsoft.UI.Xaml.Controls`, `Windows.Foundation`,
  `Windows.Win32.FileSystem`, and `Windows.Wdk.BASETSD` from the same
  compiler run.
- **C++23 modules only.** The runtime (`winrt_base`) and every projection
  are emitted as `.ixx` module interface units. Consumers `import` them
  directly — no headers, no IFC build ordering headaches. The standard
  library is a named module too: apps and the generated code use
  `import std;`, never STL headers.- **Multi-core generation.** Metadata loading, classification, WASDK NuGet
  cache discovery (metadata, runtime-DLL and bootstrap walks all run on all
  cores — one shared bootstrap scan per process) and module generation always
  use all available CPU cores (`CPPWINC_JOBS=<n>` caps it).
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

## WinUI 3 Projection

`CppWinC` provides first-class, projection-agnostic compilation for WinUI 3 desktop applications. Driven directly by ECMA-335 `.winmd` metadata containing `WinUIAttribute` AST payloads and `WinUIControlCatalogAttribute` definitions, `CppWinC` generates modular C++23 views and links against the Windows App SDK without requiring manual XAML code-behind.

### Manual Editing, Business Logic & Third-Party Control Integration

While `CppWinC` generates complete, turnkey view modules automatically from XAML AST metadata, developers maintain full control. Generated views are pure C++23 modules with zero proprietary lock-in or "black box" code generation:

#### 1. Inspecting & Manually Customizing Generated Views

Every XAML view compiles into a cleanly formatted C++23 module (`<Namespace>.<ViewName>.ixx`) that exports `winui_base` and defines an exported view class:

```cpp
// Example: generated HomePageView in WinUIGallery.HomePage.ixx
export module WinUIGallery.HomePage;

import std;
export import winui_base;
import WinUIGallery.HomePageHeader;
import WinUIGallery.HorizontalScrollContainer;

export class HomePageView : public Microsoft::UI::Xaml::Controls::UserControl
{
public:
    HomePageView() { BuildUI(); }

    // State variables (including x:Name controls promoted to state):
    winrt::Microsoft::UI::Xaml::Controls::SelectorBar FilterBar;
    // ...

    // Named XAML elements (x:Name="...") emitted as public, strongly-typed members:
    winrt::Microsoft::UI::Xaml::Controls::GridView FavoriteSamples;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel FavoriteSamplesFallbackMessage;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel FavoriteSamplesPanel;
    winrt::Microsoft::UI::Xaml::Controls::StackPanel RecentSamplesPanel;
    HorizontalScrollContainer RecentlyVisitedContainer;
    winrt::Microsoft::UI::Xaml::Controls::TextBlock RecentlyVisitedTitle;

    // Reactive bindings and child views:
    std::vector<std::function<void()>> m_bindings;
    std::vector<std::shared_ptr<void>> m_child_views;
    void update_bindings() { for (auto const& fn : m_bindings) fn(); }

    // Build the UI tree from the winuic layout (defined inline):
    void BuildUI() { /* ... */ }
};
```

Because elements are strongly typed members:
- **Direct Event Overrides & Handlers:** Hook events programmatically at runtime without touching generated XAML parser logic (`SelectionChanged` forwards directly to `add_SelectionChanged`):
  ```cpp
  auto view = std::make_shared<HomePageView>();
  view->FilterBar.SelectionChanged([](auto const& sender, auto const& args) {
      // Bespoke filtering or event handling logic
  });
  ```
- **Custom Composition & Animations:** Access WinUI composition visuals (`ElementCompositionPreview::GetElementVisual(view->RecentlyVisitedContainer)`) to run fluid spring physics, connected animations, or Win2D canvas drawing.
- **In-Place Editing:** You can directly modify `BuildUI()` inside the generated `.ixx` file to customize layout trees, insert custom UI containers, or override styles.

#### 2. Business Logic & MVVM Integration

Connect views to C++23 view models, state containers, data binding models (`INotifyPropertyChanged`, `IObservableVector`), and background worker coroutines (`co_await winrt::impl::resume_background()`):

```cpp
// Sample C++23 ViewModel module:
export module MyApp.SampleViewModel;

import std;
import winrt_base;
import winrt.Windows.Foundation;
import winrt.Windows.Foundation.Collections;
import winrt.Microsoft.UI.Xaml;
import winrt.Microsoft.UI.Dispatching;

export struct SampleViewModel : winrt::implements<SampleViewModel,
    winrt::Microsoft::UI::Xaml::Data::INotifyPropertyChanged>
{
    winrt::hstring m_title{ L"Initial Title" };
    winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::IInspectable> m_items =
        winrt::single_threaded_observable_vector<winrt::IInspectable>();

    winrt::event_token PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler) {
        return m_propertyChanged.add(handler);
    }
    void PropertyChanged(winrt::event_token const& token) noexcept {
        m_propertyChanged.remove(token);
    }

    winrt::hstring Title() const { return m_title; }
    void Title(winrt::hstring const& value) {
        if (m_title != value) {
            m_title = value;
            m_propertyChanged.invoke(
                get_strong().as<winrt::IInspectable>(),
                winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ L"Title" });
        }
    }

    winrt::Windows::Foundation::Collections::IObservableVector<winrt::IInspectable> Items() const {
        return m_items;
    }

    // Coroutine offloading intensive work to thread pool:
    winrt::impl::cancellable_async_action LoadDataAsync(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue) {
        co_await winrt::impl::resume_background(); // Worker thread

        std::vector<winrt::hstring> loaded;
        for (int i = 1; i <= 50; ++i) {
            loaded.emplace_back(winrt::to_hstring(std::format("Data Record #{}", i)).c_str());
        }

        co_await winrt::impl::resume_foreground(queue); // Dispatch back to UI thread
        for (auto const& item : loaded) {
            m_items.Append(winrt::IInspectable(winrt::box_value(item)));
        }
    }
};
```

##### Connecting the ViewModel to the View (Same File vs. New File)

You can connect your ViewModel to the View using either of two patterns:

**1. In a Separate File / Host Controller (Recommended for Production & Non-Destructive Regeneration)**
- **Why:** Re-running `cppwinc` into `-output` updates and overwrites the generated view `.ixx` files when XAML metadata changes. Placing your business logic, ViewModel definitions, and event wiring in a separate companion file (e.g. `SampleViewModel.ixx`, `HomePageController.cpp`, or inside your navigation container in `MainWindow.ixx`) ensures your custom C++ code is never overwritten during UI regeneration.
- **Implementation:**
  ```cpp
  // In a separate host/controller file (e.g., PageNavigator.cpp or MainWindow.ixx):
  import MyApp.HomePage;
  import MyApp.SampleViewModel;

  void NavigateToHomePage(Microsoft::UI::Xaml::Controls::Frame const& contentFrame) {
      auto view = std::make_shared<HomePageView>();
      auto viewModel = winrt::make_self<SampleViewModel>();

      // Bind ViewModel to View's DataContext:
      view->DataContext(viewModel.as<winrt::IInspectable>());

      // Trigger background coroutine worker:
      winrt::attach_cancellable(viewModel->LoadDataAsync(view->DispatcherQueue()));

      contentFrame.Content(*view);
  }
  ```

**2. Directly Inside the Generated View File (`<ViewName>.ixx`)**
- **Why:** Convenient when treating `cppwinc` output as a one-time project scaffold or when maintaining the view file directly as authoritative source code.
- **Implementation:** Import your ViewModel module at the top of `<ViewName>.ixx`, instantiate it inside the View's constructor right after `BuildUI()`, and assign `DataContext`:
  ```cpp
  // Inside the generated <ViewName>.ixx:
  import MyApp.SampleViewModel;

  export class HomePageView : public Microsoft::UI::Xaml::Controls::UserControl
  {
  public:
      HomePageView() {
          BuildUI();

          // Connect ViewModel directly in the View's constructor:
          auto vm = winrt::make_self<SampleViewModel>();
          DataContext(vm.as<winrt::IInspectable>());
          winrt::attach_cancellable(vm->LoadDataAsync(DispatcherQueue()));
      }
      // ...
  };
  ```

#### 3. Third-Party & In-House Controls Integration

Consume third-party or in-house component suites (vendor controls, open-source libraries, or proprietary internal controls) completely generically with zero hardcoded compiler rules:

- **Modify built-in controls / add your own C++** — put headers and sources under `externals/` and `#include` them from any module; they compile with the same flags as everything else, and their objects join the same links.
- **Mix in external NuGet third-party controls** — drop the package under `externals/` for its headers/libs/DLLs and pass its `.winmd` manually as a regular `-input` entry (metadata is never auto-discovered). When your own winuic-produced winmd is present in `-input`, every other winmd is treated as a library: its types are projected as ordinary platform types — their namespaces are imported, kept alive by the needed-set, and never misfiled as app/component types.
- **Create new controls** — write the CppWinC control, pass its `.winmd` via `-input`, drop its binaries under `externals/`, and describe it in a `*.json` catalog next to `winuic.exe` (auto-loaded) if you also want the `.winui` DSL to see it. Downstream, the `.winmd` you consume is language-projection agnostic — winuic/xaml2winuic emit metadata only, and this compiler turns it into C++23 modules.

##### Step-by-Step Integration Workflow:

1. **Drop Binaries into `externals/`:**
   `cppwinc` creates the top-level `externals/` directory only when it is absent. If it already exists, it scans it without changing its nested files or folders:
   ```
   externals/
   ├── include/                               <-- C++ headers (added to /I automatically)
   ├── <ThirdPartyControls>.lib               <-- Import libraries linked automatically
   └── <ThirdPartyControls>.dll               <-- Copied adjacent to final executable
   ```
2. **Provide Metadata to `-input`:**
   Pass the control's `.winmd` file to `cppwinc`:
   ```shell
   cppwinc -input "winuic_out\MyApp.winmd,winmd\Windows.Win32.winmd,externals\<ThirdPartyControls>.winmd" -winui exp -output "out"
   ```
3. **Automatic Resource Discovery & Direct Consumption:**
   `cppwinc` generically inspects the `.winmd` TypeDef tables, automatically discovers and merges any embedded `ResourceDictionary` classes into `Application.Current.Resources.MergedDictionaries` at launch, and projects the controls as strongly-typed C++23 modules:
   ```cpp
   import winrt.<ThirdPartyNamespace>;

   // Third-party controls are fully projected and ready to use:
   winrt::<ThirdPartyNamespace>::<CustomControl> control;
   control.Header(L"Custom Component");
   ```

### Metadata-First WinUI 3 Architecture & Rebuild-Free Evolution

All control resolution, type system discovery, container layouts, dimension sizing, property evaluation, and resource handling are driven dynamically from ECMA-335 `.winmd` metadata:

- **Universal Control & Layout Resolution**: The compiler embeds no hardcoded lists of UI controls, panels, or layout containers. Any standard, third-party, custom, or future control (such as future WinUI Gallery releases or new Windows App SDK versions) resolves automatically through its metadata TypeDef, interface hierarchies, and `WinUIControlCatalogAttribute` schema.
- **Generic Layout & Dimension Sizing**: Layout properties, dimensions (`Width`, `Height`, `MinWidth`, `MaxHeight`), alignment vectors (`HorizontalAlignment`, `VerticalAlignment`), and container-specific sizing definitions (e.g. Grid `Star`, `Auto`, `Pixel`, `GridLength`, and RelativePanel constraints) resolve generically based on the target property type system in metadata, allowing arbitrary nested layouts and containers to size and align correctly.
- **Dynamic Closure Root Discovery**: Rather than relying on a static set of controls, `cppwinc` parses the `WinUIAttribute` AST payloads from view classes in `.winmd`, extracts all referenced control and type names, matches them against `g_type_ns` in the metadata reader, and dynamically injects them into the module compilation `closure_roots`.
- **Dynamic Resource Dictionary Integration & Day-1 Unknown Control Discovery**: Controls requiring specialized platform resource dictionaries or external styles are dynamically resolved and instantiated via `winrt::impl::create_instance` and merged into `Application.Current.Resources.MergedDictionaries` at launch. Rather than hardcoding known platform resource dictionaries, `cppwinc` dynamically inspects the `WinUIControlCatalogAttribute` schema from `.winmd` (reading top-level `resource_dictionaries` and per-control `resource_dictionary` declarations) as well as querying all loaded `.winmd` ECMA-335 `TypeDef` tables for any classes ending in `Resources` that derive from `ResourceDictionary`. Discovered resource classes are automatically injected into module `closure_roots` and merged into `MergedDictionaries` at launch, completely decoupling control resources from compiler updates and guaranteeing 100% Day-1 support for any future or unknown controls.
- **Type-Driven Events & Properties**:
  - **Events** are resolved dynamically from the winmd `Event` table, flattened across all implemented base classes and interfaces.
  - **Properties** are resolved from `PropertyDef` rows and getter/setter method signatures. Codegen gates strictly on the property's *underlying metadata type* (primitives, numeric types, enums, strings, brushes, geometries, thicknesses) rather than control names, ensuring full type safety and zero rebuilds for future APIs.
- **Rebuild-Free Evolution**: Because the entire projection pipeline is generic and metadata-driven, new controls, properties, events, and layouts function seamlessly out of the box with zero compiler rebuilds.

### Brush and Color Handling

WinUI 3 brush and color properties are projected dynamically with full conversion support:
- **Hex Color Strings (`#RGB`, `#ARGB`, `#RRGGBB`, `#AARRGGBB`)**: Parsed at codegen into 32-bit ARGB values and wrapped in a `SolidColorBrush` via `make_solid_brush(Color{A, R, G, B})`.
- **Named and Transparent Brushes**: Named colors and `"Transparent"` resolve directly to alpha-zero `Color{0, 0, 0, 0}` solid brushes.
- **ThemeResource and StaticResource Brushes**: Property setters referencing `{ThemeResource ...}` or `{StaticResource ...}` (e.g. `Foreground="{ThemeResource ButtonForeground}"`) resolve dynamically via runtime resource lookup (`theme_lookup_th` / `resource_lookup_th`) across the application and theme dictionaries.
- **Property Attribution**: Properties typed as `Microsoft.UI.Xaml.Media.Brush` (e.g. `Foreground`, `Background`, `BorderBrush`, `Fill`, `Stroke`) are safely emitted with SFINAE-guarded setters, preventing empty string or resource-tag misassignment.

### Geometry and SVG Path Handling

Shapes, paths, and vector icons are fully projected without external dependency:
- **SVG Path Parsing**: Path data strings (such as `M... C... c... a... z` SVG paths) in `PathIcon.Data` or `Path.Data` are parsed at runtime into native WinUI `Geometry` objects via `parse_svg_path(data)`.
- **Placeholder Geometries**: Properties typed as `Microsoft.UI.Xaml.Media.Geometry` without direct string path data emit safe default `Geometry()` placeholders.
- **Icon Controls**: Full integration for `PathIcon` (vector paths), `FontIcon` (Segoe Fluent glyphs), `SymbolIcon` (standard symbol enumerations), `BitmapIcon`, and `AnimatedIcon`.

### Font and Icon Handling

- **Font Family Resolution**: Properties typed as `FontFamily` accept string family names (e.g. `Segoe UI`, `Segoe Fluent Icons`) or `{ThemeResource SymbolThemeFontFamily}` and construct `FontFamily(name)`.
- **Glyph Attribution**: Unicode glyph codepoints (including private-use Segoe Fluent icon glyphs like ``, ``, ``) passed to `FontIcon.Glyph` are forwarded via explicit `winrt::hstring` conversion, ensuring no silent SFINAE discard during C++ template deduction.

### Dynamic Resource Dictionary & Theme Lookup

Resource lookups dynamically search the WinUI 3 resource hierarchy:
- **Dual Map Interface Query**: WinUI 3 `ResourceDictionary` objects are queried against both WinRT `IMap<IInspectable, IInspectable>` (`0xf5f69427-...`) and `IMap<HSTRING, IInspectable>` (`0x1b0d3570-...`) ABI interfaces, ensuring compatibility across all Windows App SDK versions.
- **Theme Dictionaries**: Recursive resource traversal (up to 15 levels deep) automatically inspects active `ThemeDictionaries` (`Light`, `Dark`, `HighContrast`) before falling back to application-level resources or default styling.

### Untyped Item Property Access & DataTemplate Handling

- **DataTemplate Property Access**: When data-binding untyped collection items (`item.Property` on `IInspectable`), the compiler guards property access to avoid type-mismatch crashes on heterogeneous collections.

### Shared WinUI 3 Base Module (`winui_base`)

For WinUI 3 builds, `CppWinC` emits a shared `winui_base.ixx` (`export module winui_base;`) that every generated view module re-exports (`export import winui_base;`). Instead of repeating platform imports, theme helpers, and component stubs in every view `.ixx`, `winui_base` centralizes:
- **Platform & Runtime Re-exports:** `Microsoft.UI.Xaml.*`, `Microsoft.UI.Dispatching`, `Microsoft.UI.Windowing`, `Windows.Foundation.*`, `Windows.UI.*`, `Windows.Win32.UI.WindowsAndMessaging`, and the app's own component module.
- **Shared Theme & Resource Runtime (`ThemeState`):** `theme_lookup_th`, `theme_brush_th`, `style_lookup_th`, `data_template_th`, `apply_xaml_resources`, `apply_visual_state_groups`, `symbol_from_name`, `parse_svg_path`, and recent-history persistence.
- **Metadata-Driven Component Stubs & Helpers:** Emits projection-agnostic helper wrappers and fallback stubs discovered from `.winmd` metadata once in `winui_base` rather than duplicating them across individual view modules.

Set `CPPWINC_DUMP_CTRL_MAPS=1` to print the derived maps at startup.

## WinRT Projection

### Runtime Helpers (`winrt_base`)

Everything an app needs beyond the metadata projection lives in `winrt_base.ixx` (`module winrt_base;`), emitted by `CppWinC` from `strings_data.ixx`. It delivers a complete, headerless C++23 WinRT runtime layer:

- **String Types & Conversions:**
  - `hstring` — immutable UTF-16 string with atomic reference counting. Supports `size()`, `empty()`, `c_str()`, iterators, concatenation (`+`, `+=`), equality/comparison operators, and implicit conversions to/from `std::wstring`, `std::wstring_view`, `wchar_t const*`, and `std::string_view`.
  - `hstring_builder` — high-performance string builder for piecewise UTF-16 construction.
  - `to_hstring(std::string_view)` / `to_hstring(std::wstring_view)` / `to_hstring(char const*)` / `to_hstring(wchar_t const*)` — converts narrow or wide strings to `hstring`.
  - `to_string(hstring const&)` — converts UTF-16 `hstring` to UTF-8 `std::string`.
  - `to_wstring(hstring const&)` — converts `hstring` to `std::wstring`.

- **GUIDs & Type Identification:**
  - `winrt_guid` — 128-bit GUID value type with equality, hashing (`std::hash`), string serialization, and conversion to/from standard Windows `GUID`.
  - `null_guid` — `constexpr winrt_guid{ 0 }` sentinel.
  - `iid_of<T>` — template variable resolving the 128-bit interface identifier for any projected WinRT interface or delegate.
  - `make_generic_iid(pattern, args, count)` — calculates the parameterized interface GUID at runtime using SHA-1 hashing according to the ECMA-335 / WinRT specification.

- **COM & Smart Pointers:**
  - `com_ptr<T>` — intrusive, atomic reference-counted smart pointer for WinRT and COM interfaces.
    - Automatic `AddRef()` on copy and `Release()` on destruction.
    - `put_abi()` — clears current reference and returns `void**` for out-parameters.
    - `get_abi()` — returns raw underlying interface pointer without affecting ref count.
    - `detach_abi()` — releases ownership and returns raw pointer.
    - `attach_abi(raw)` — assumes ownership of an existing raw interface pointer.
    - `copy_from(raw)` — AddRefs and assigns a raw pointer.
    - `copy_to(out)` — AddRefs and writes pointer to an out-parameter.
    - `as<U>()` / `try_as<U>()` — queries interface `U` from `T` via `QueryInterface`.
  - `IUnknown` / `IInspectable` — projected base interfaces with ABI query, AddRef, Release, and runtime class name interrogation (`GetRuntimeClassName`).

- **Error Handling & Exceptions:**
  - `hresult_error` — exception class wrapping an `HRESULT` with message formatting and error origin tracking.
  - Typed exception subclasses:
    - `hresult_access_denied` (`0x80070005`)
    - `hresult_invalid_argument` (`0x80070057`)
    - `hresult_out_of_memory` (`0x8007000E`)
    - `hresult_canceled` (`0x800704C7`)
    - `hresult_illegal_method_call` (`0x8000000E`)
    - `hresult_not_implemented` (`0x80004001`)
    - `hresult_changed_state`
    - `hresult_class_not_available`
    - `hresult_class_not_registered`
    - `hresult_illegal_delegate_assignment`
    - `hresult_illegal_state_change`
    - `hresult_no_interface` (`0x80004002`)
    - `hresult_out_of_bounds`
    - `hresult_wrong_thread`
    - `hresult_type_load`
  - `check_hresult(HRESULT)` — verifies success; throws the matching typed exception on failure.
  - `check_bool(bool)` — throws `hresult_invalid_argument` or last error if false.
  - `check_win32(error)` — validates Win32 error code and throws on failure.
  - `check_nt(status)` — validates NTSTATUS code and throws on failure.
  - `check_hresult_allow_bounds(HRESULT)` — allows out-of-bounds sentinel without throwing.
  - `throw_hresult(HRESULT)` — unconditionally throws the typed error.
  - `throw_last_error()` — converts Win32 `GetLastError()` to HRESULT and throws.
  - `hresult_from_win32(error)` — translates a Win32 error code to an HRESULT (`HRESULT_FROM_WIN32`).
  - `hresult_from_nt(status)` — translates an NTSTATUS code to an HRESULT (`HRESULT_FROM_NT`).
  - `to_hresult()` / `to_hresult(exception_ptr)` — extracts HRESULT from active C++ exception or structured exception.
  - `catch_to_hresult()` — catches any in-flight C++ exception and converts it to the equivalent HRESULT.

- **Apartment & Concurrency:**
  - `apartment_type` — `single_threaded` (STA) or `multi_threaded` (MTA).
  - `init_apartment(type = multi_threaded)` — initializes COM/WinRT apartment for the current thread.
  - `uninit_apartment()` — closes the apartment.
  - `is_sta_thread()` — inspects whether the current thread is in an STA.
  - `apartment_context` — captures current apartment and dispatches resumption onto it.
  - `thread_pool` — WinRT system thread pool dispatcher.

- **Coroutines & Asynchronous Programming:**
  - `cancellable_async_action` / `cancellable_async_operation<T>` — coroutine return types with full cooperative cancellation support.
  - `attach_cancellable(coro)` — wraps internal coroutines into projected `IAsyncAction` / `IAsyncOperation<T>` interfaces for external callers.
  - `resume_background()` — awaiter that switches coroutine execution to a thread-pool worker thread.
  - `resume_apartment()` / `resume_foreground(dispatcher)` — awaiter switching execution to the UI thread or original apartment.
  - `fire_and_forget` — standard fire-and-forget coroutine return type (no caller synchronization required).
  - `when_all(...)` — async combinator awaiting completion of all supplied operations.
  - `when_any(...)` — async combinator awaiting completion of the first supplied operation.

- **Boxing, Unboxing & PropertyValues:**
  - `box_value(val)` — boxes fundamental types (`bool`, `int8`..`int64`, `uint8`..`uint64`, `float`, `double`, `char16_t`, `hstring`, `winrt_guid`, string literals) into an `IInspectable` PropertyValue.
  - `unbox_value<T>(inspectable)` — unboxes an `IInspectable` back to type `T` (throws on type mismatch).
  - `try_unbox<T>(inspectable)` — non-throwing unbox returning `std::optional<T>`.
  - `unbox_value_at<T>(map, key)` — convenience helper to lookup and unbox from `IMap<hstring, IInspectable>`.
  - `property_value_factory()` — cached accessor to `IPropertyValueStatics`.

- **Collections & Data Structures:**
  - `single_threaded_vector<T>()` — instantiates in-memory `IVector<T>` / `IVectorView<T>`.
  - `single_threaded_observable_vector<T>()` — instantiates `IObservableVector<T>` with change notifications.
  - `single_threaded_map<K, V>()` — instantiates in-memory `IMap<K, V>` / `IMapView<K, V>`.
  - `single_threaded_observable_map<K, V>()` — instantiates `IObservableMap<K, V>` with change notifications.
  - `IReserveableVector<T>` — capacity-reservation extension interface for `IVector<T>` (`Reserve(capacity)`), allowing preallocation before bulk appends without repeated reallocation penalties (the WinRT equivalent of `std::vector::reserve`). Every vector created by the runtime (`single_threaded_vector`, `single_threaded_observable_vector`) implements this interface.
    > **Provisional IID Warning:** `IReserveableVector` is an extension interface **not** officially defined or shipped by Microsoft in standard WinRT metadata. Its interface ID (`guid_ireserveablevector = { 0x3f7a19c2, 0x6b1e, 0x4d95, { 0x8a, 0x24, 0xd1, 0x5e, 0x9c, 0x47, 0xb3, 0x08 } }`) is **provisional** and defined exclusively in `CppWinC`'s `strings_data.ixx`. It is subject to change if Microsoft or the WinRT specification officially assigns a standardized IID in the future.
  - Collection GUIDs & type tokens: `guid_iiterable`, `guid_iiterator`, `guid_ivector`, `guid_ivectorview`, `guid_iobservablevector`, `guid_ireserveablevector`, `guid_imap`, `guid_imapview`, `guid_iobservablemap`, `guid_ikeyvaluepair`, `guid_ireference`.
  - Notification change codes: `collection_change_reset`, `collection_change_item_inserted`, `collection_change_item_removed`, `collection_change_item_changed`.
  - Change event args: `vector_changed_args<T>`, `map_changed_args<K>`.

- **Events & Delegates:**
  - Delegates — templated functor wrappers providing `operator()` and comparison for WinRT callbacks.
  - `event<Delegate>` — type-safe multi-cast event source supporting registration (`+=`), unregistration (`-=`), and invocation.
  - `event_token` — 64-bit registration token returned upon adding a handler.
  - `auto_revoke_t` / `auto_revoke` — tag constant for automatic event unregistration.
  - `event_revoker<I>` — RAII auto-revoker unregistering handlers on destruction.

- **Component Authoring & Class Implementation:**
  - `implements<D, Interfaces...>` — CRTP base class for authoring custom WinRT runtime classes and interfaces, handling ref counts, `QueryInterface`, and inspectable metadata.
  - `interface_impl_base<I>` — base class for single interface implementations.
  - `atomic_ref_count` — atomic thread-safe reference counter used in WinRT classes.
  - `make<T>(args...)` — allocates a runtime class and returns its default projected interface.
  - `make_self<T>(args...)` — allocates a runtime class and returns a `com_ptr<T>` to the implementation type.
  - `get_self<T>(projected)` — extracts the underlying C++ implementation pointer from a projected wrapper.
  - `non_agile` / `no_weak_ref` — marker mixins for opting out of agile marshaling or weak reference tracking.
  - `create_instance(class_name)` — activates a runtime class dynamically via `RoGetActivationFactory`.
  - `register_local_factory(name, fn)` / `try_get_factory(name)` — in-process factory cache for self-hosted components.
  - `get_factory(name)` / `create_fallback_element()` — factory resolution and fallback instantiation.
  - `factory_cache<Class, Interface>` — thread-safe activation factory caching helper.
  - `iface_cache` — per-instance interface query caching.
  - `deferral_handler` / `iid_of_deferral` — async deferral management helper.

- **Synchronization & Windows Handles:**
  - `handle_type` / `handle` — RAII wrapper around Win32 `HANDLE`, automatically calling `CloseHandle` on destruction.
  - `slim_mutex` — lightweight user-mode SRW-based mutex wrapper.
  - `slim_lock_guard` — RAII exclusive lock guard for `slim_mutex`.
  - `slim_shared_lock_guard` — RAII shared read lock guard for `slim_mutex`.
  - `slim_condition_variable` — CONDITION_VARIABLE wrapper.

- **Clock & Time Conversions:**
  - `clock` — WinRT 100-nanosecond intervals clock type.
  - `file_time` — `clock::time_point` representation.
  - `from_file_time(FILETIME)` / `to_file_time(time_point)` — conversions to/from Windows `FILETIME`.
  - `from_sys(time_point)` / `to_sys(time_point)` — conversions to/from `std::chrono::system_clock`.

- **Nullability & Safe ABI Borrowing:**
  - `explicit operator bool() const noexcept`, `bool operator!() const noexcept`, `operator==(std::nullptr_t)`, and `operator!=(std::nullptr_t)` on projected runtime classes, `IUnknown`, and `IInspectable` — allowing idiomatic `if (!ctrl)` and `if (ctrl == nullptr)` checks without implicit conversion ambiguity.
  - `winrt::impl::borrow_arg_in<T>(raw)` — constructs a properly zero-initialized `wrap_borrowed` wrapper for runtime-class parameters inside generated delegate (`TypedEventHandler`) and component thunks, preventing uninitialized stack reads into `iface_cache`.

- **Diagnostics & Process Lifecycle:**
  - `boot_log(msg)` / `boot_logf(fmt, ...)` — low-level trace logging.
  - `crash_report_current_exception()` / `set_crash_reporter()` — automated exception crash reporting with stack and HRESULT diagnostics.

> **Note:** `winrt_base` is only used by WinRT and WinUI projection modules.
> Win32 and WDK modules do not import `winrt_base` — they use plain C++ types.

### Component Module Structs

The component module (`<Name>.ixx`) exports a struct per WinRT runtime class.
`class_name()` carries the runtime class's metadata full name **verbatim** —
including the hyphenated app namespace (`My App` → `My-App`) — because the
activation key must match the `.winmd`:

```cpp
export struct MainWindow {
    static constexpr std::wstring_view class_name() noexcept { return L"My-App.MainWindow"; }
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

### Example — Async file I/O

```cpp
import std;
import winrt_base;
import winrt.Windows.Foundation;
import winrt.Windows.Storage;

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;

int main() {
    auto file = StorageFile::GetFileFromPathAsync(L"data.txt").get();
    auto stream = file.OpenReadAsync().get();

    // OpenReadAsync yields IRandomAccessStreamWithContentType; DataReader's
    // constructor wants IInputStream - query the inherited interface.
    void* raw_in = nullptr;
    winrt::check_hresult(winrt::impl::query_interface(stream.get_abi(), winrt::iid_of<IInputStream>, &raw_in));
    DataReader reader = DataReader::from_abi(raw_in);

    // DataReaderLoadOperation is a runtime class over the generic
    // IAsyncOperation<uint32>; query it to block on completion.
    auto load = reader.LoadAsync(1024);
    void* raw_load = load.iface<IAsyncOperation<std::uint32_t>>();
    IUnknown::addref_raw(raw_load);
    IAsyncOperation<std::uint32_t> load_op = IAsyncOperation<std::uint32_t>::from_abi(raw_load);
    load_op.get();

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
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Foundation::Collections;

int main() {
    // PropertySet is a runtime class; its default interface is the generic
    // IMap<hstring, IInspectable>, queried out of the class wrapper.
    PropertySet props;
    void* raw = props.iface<IMap<hstring, IInspectable>>();
    IUnknown::addref_raw(raw);
    auto map = IMap<hstring, IInspectable>::from_abi(raw);

    map.Insert(L"Width", IInspectable(box_value(800)));
    map.Insert(L"Height", IInspectable(box_value(600)));
    map.Insert(L"Title", IInspectable(box_value(hstring(L"CppWinC"))));

    std::cout << "Width = " << unbox_value<int32_t>(map.Lookup(L"Width")) << std::endl;
    std::cout << "Height = " << unbox_value_at<int32_t>(map, L"Height") << std::endl;
    std::wcout << L"Title = " << std::wstring(unbox_value<hstring>(map.Lookup(L"Title"))) << std::endl;

    map.Remove(L"Title");
    std::cout << "size after Remove: " << map.get_Size() << std::endl;
}
```

### Example — WinRT coroutines

```cpp
import std;
import winrt_base;
import winrt.Windows.Foundation;

// Coroutines return cancellable operations, wrapped into the projected
// async interface at the API boundary.
winrt::impl::cancellable_async_operation<int> compute_async() {
    co_await winrt::impl::resume_background();
    co_return 42;
}

int main() {
    int result = winrt::attach_cancellable(compute_async()).get();
    std::cout << "Result: " << result << std::endl;
}
```

## Win32 API Projection

`CppWinC` consumes `Windows.Win32.winmd` produced by
[winversec-compiler](https://github.com/mominshaikhdev/winversec-compiler)
and generates type-safe C++23 module projections for all Win32 APIs.

Win32 modules import **no runtime** — plain C++ types with `extern "C"`
linkage, `__stdcall` calling conventions, and `void*` handles (ownership
stays with the caller).

### What is projected

| Category | Examples | Module |
|---|---|---|
| **File system** | `CreateFile`, `ReadFile`, `WriteFile`, `FindFirstFile` | `Windows.Win32.Storage.FileSystem` |
| **Registry** | `RegOpenKeyEx`, `RegQueryValueEx`, `RegSetValueEx` | `Windows.Win32.System.Registry` |
| **Window management** | `CreateWindowEx`, `DefWindowProc`, `GetMessage` | `Windows.Win32.UI.WindowsAndMessaging` |
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

- **Handles.** `HANDLE`, `HWND`, `HKEY`, `HMODULE`, `SC_HANDLE`, `SOCKET`
  and friends flatten to `void*` — pass raw pointers and release with the
  projected `CloseHandle`/`RegCloseKey`-style functions.
- **Buffer marshaling.** SAL annotations drive automatic buffer marshaling.
- **Calling convention.** `WINAPI`/`CALLBACK`/`cdecl`/`stdcall` projected
  as `__stdcall`, `__cdecl`, etc.
- **Import library.** `DllImport`/`ImportLibrary` is recorded on every
  `extern "C"` declaration as a `// Import library:` attribution, and
  `-compile` quick-tests read those attributions to link the right `.lib`s
  automatically.
- **A/W resolution.** every `FooA`/`FooW` pair gets an unsuffixed
  `export inline` canonical `Foo` that forwards to the W (Unicode) variant —
  `CreateFile`, `RegOpenKeyEx`, `RegisterClass`. String parameters are
  re-spelled `wchar_t const*`/`wchar_t*` in the canonical, so `L"..."`
  arguments bind directly. Where the metadata already synthesized a stub
  canonical (`GetMessage`), the stub is dropped and the forwarder takes its
  place.
- **EnumFlags.** Bitmask enums get `|`, `&`, `~`, `|=`, `&=` operators.
- **StructLayout/Union.** Sequential, explicit, and union layouts with
  custom packing projected with correct memory layout.
- **Inline functions.** Functions marked `InlineFunction` emit inline bodies.

### Example — Win32 file I/O

```cpp
import std;
import Windows.Win32.Storage.FileSystem;
import Windows.Win32.System;
import Windows.Win32.Foundation;

int main() {
    using namespace Windows::Win32::Storage::FileSystem;
    using namespace Windows::Win32::System::SystemServices;

    // The canonical CreateFile forwards to CreateFileW, so L"..." binds
    // to wchar_t const* directly. Win32 constants project as inline
    // constexpr values in their metadata namespaces.
    void* file = CreateFile(L"data.txt", GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    char buf[4096]{};
    std::uint32_t bytesRead = 0;
    ReadFile(file, buf, sizeof(buf), &bytesRead, nullptr);
    Windows::Win32::Foundation::CloseHandle(file);

    std::cout << "Read " << bytesRead << " bytes" << std::endl;
}
```

### Example — Win32 registry

```cpp
import std;
import Windows.Win32.System.Registry;
import Windows.Win32.System;

int main() {
    using namespace Windows::Win32::System::Registry;
    using namespace Windows::Win32::System::SystemServices; // KEY_READ, REG_OPTION_NON_VOLATILE

    // Predefined root keys and access masks project as inline constexpr
    // values (HKEY_CURRENT_USER == 0x80000001, KEY_READ == 0x00010000).
    void* hkey_current_user = reinterpret_cast<void*>(static_cast<std::uintptr_t>(HKEY_CURRENT_USER));
    void* key = nullptr;
    RegOpenKeyEx(hkey_current_user, L"Software\\MyApp", REG_OPTION_NON_VOLATILE, KEY_READ, &key);

    char value[256]{};
    std::uint32_t size = sizeof(value);
    RegQueryValueEx(key, L"InstallPath", nullptr, nullptr,
        reinterpret_cast<std::uint8_t*>(value), &size);
    RegCloseKey(key);
}
```

### Example — Win32 window creation

```cpp
import std;
import Windows.Win32.UI.WindowsAndMessaging;
import Windows.Win32.Graphics.Gdi;

using namespace Windows::Win32::UI::WindowsAndMessaging;
using namespace Windows::Win32::Graphics::Gdi;

// Handle/scalar aliases are flattened: HWND is void*, WPARAM is uint64_t,
// LRESULT is int64_t. WNDCLASSW lives under its metadata name tagWNDCLASSW
// with void* fields, and WNDPROC is the function-pointer type to cast to.
std::int64_t __stdcall WndProc(void* hwnd, std::uint32_t msg, std::uint64_t wp, std::int64_t lp) {
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int main() {
    tagWNDCLASSW wc{};
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(WndProc);
    wc.lpszClassName = (void*)L"Win32App";
    wc.hbrBackground = GetStockObject(WHITE_BRUSH);
    RegisterClass(&wc);

    void* hwnd = CreateWindowEx(WS_EX_OVERLAPPEDWINDOW, L"Win32App", L"Hello Win32",
        WS_OVERLAPPEDWINDOW, static_cast<std::int32_t>(CW_USEDEFAULT),
        static_cast<std::int32_t>(CW_USEDEFAULT), 800, 600, nullptr, nullptr, nullptr, nullptr);

    ShowWindow(hwnd, SW_SHOW);

    tagMSG msg{};
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

The current WDK `.winmd` covers the kernel surface — `ntddk.h`, `wdm.h`,
`fltKernel.h`, `ndis.h`, `storport.h`, the rest of `km/` and `shared/`
(type-safe integer conversion macros, string safety helpers, RPC/NDR
marshaling, network extension helpers), and the KMDF/UMDF WDF framework
trees (`Include\wdf\{kmdf,umdf}`: `WdfDeviceCreate` and friends).

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
import std;
import Windows.Wdk.BASETSD;

int main() {
    using namespace Windows::Wdk::BASETSD;

    void* handle = nullptr;
    std::int32_t value = HandleToLong(handle);
    void* restored = LongToHandle(value);

    // PtrToInt/IntToPtr work on void* in this projection.
    void* ptr = nullptr;
    std::int32_t as_int = PtrToInt(ptr);
    void* from_int = IntToPtr(as_int);
    (void)restored; (void)from_int;
}
```

### Example — String safety (STRSAFE)

```cpp
import std;
import Windows.Wdk.STRSAFE;

int main() {
    using namespace Windows::Wdk::STRSAFE;

    // The A/W canonicals take the W (wchar_t) spellings: destination
    // buffer, its size, and a mutable source string.
    wchar_t dest[256]{};
    wchar_t src[] = L"Hello from CppWinC";
    StringCchCopy(dest, 256, src);
    StringCbCopy(dest, sizeof(dest), src);
    std::wcout << dest << std::endl;
}
```

### Example — Network extensions (PACKET / RING)

```cpp
import std;
import Windows.Wdk.PACKET;
import Windows.Wdk.RING;

int main() {
    using namespace Windows::Wdk::PACKET;
    using namespace Windows::Wdk::RING;

    // NET_RING descriptor ring for packet queues
    NET_RING ring{};
    ring.NumberOfElements = 256;
    ring.ElementStride = sizeof(NET_PACKET);
    ring.BeginIndex = 0;
    ring.EndIndex = 64;

    // Advance ring indices or inspect packet descriptors
    std::uint32_t available = (ring.EndIndex >= ring.BeginIndex)
        ? (ring.EndIndex - ring.BeginIndex)
        : (ring.NumberOfElements - ring.BeginIndex + ring.EndIndex);

    std::cout << "Available packets in ring: " << available << std::endl;
}
```

### Example — RPC client (RPCDCE)

```cpp
import std;
import Windows.Wdk.RPCDCE;
import Windows.Wdk.RPC;
import Windows.Wdk.RPCNTERR;

int main() {
    using namespace Windows::Wdk::RPCDCE;
    using namespace Windows::Wdk::RPC;
    using namespace Windows::Wdk::RPCNTERR;

    RPC_BINDING_HANDLE binding = nullptr;
    // RPC_STATUS projects as std::int32_t, RPC_BINDING_HANDLE as void*;
    // the unsuffixed RpcBindingFromStringBinding is the metadata canonical.
    RPC_STATUS status = RpcBindingFromStringBinding(
        reinterpret_cast<std::uint8_t*>(const_cast<char*>("ncalrpc:[endpoint]")), &binding, nullptr);
    if (status == RPC_S_OK) {
        RpcBindingFree(nullptr, &binding);
    }
}
```

### Example — TPM base services (TBS)

```cpp
import std;
import Windows.Wdk.TBS;

int main() {
    using namespace Windows::Wdk::TBS;

    void* context = nullptr;
    tdTBS_CONTEXT_PARAMS params{};
    params.version = 2;
    Tbsi_Context_Create(&params, &context); // note: projected name is Tbsi_, not the TBSi_ macro alias
    // Submit TPM commands...
    Tbsip_Context_Close(context);
}
```

## Combined WinUI 3 + WinRT + Win32 + WDK

### Generate all four projections

```bat
cppwinc.exe -input local,Windows.Win32.winmd,Windows.Wdk.winmd,MyApp.winmd -base -output out

:: add WinUI 3 (WASDK `Microsoft.UI.*`): -winui auto-discovers the WASDK
:: metadata + runtime DLLs and builds it alongside WinRT/Win32/WDK
cppwinc.exe -input local,Windows.Win32.winmd,Windows.Wdk.winmd,MyApp.winmd -base -winui -output out
```

### Example — WinRT async + Win32 file I/O

```cpp
import std;
import winrt_base;
import winrt.Windows.Foundation;
import Windows.Win32.Storage.FileSystem;
import Windows.Win32.System;
import Windows.Win32.Foundation;

// The projection's coroutine machinery: a cancellable coroutine wrapped into
// the projected IAsyncAction interface at the API boundary.
winrt::impl::cancellable_async_action SaveDataAsync(std::wstring const& path, std::string const& data) {
    co_await winrt::impl::resume_background();

    using namespace ::Windows::Win32::Storage::FileSystem;
    using namespace ::Windows::Win32::System::SystemServices;
    void* file = CreateFile(path.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == reinterpret_cast<void*>(static_cast<std::intptr_t>(-1))) co_return; // INVALID_HANDLE_VALUE

    std::uint32_t written = 0;
    WriteFile(file, (void*)data.c_str(), static_cast<std::uint32_t>(data.size()),
        &written, nullptr);
    ::Windows::Win32::Foundation::CloseHandle(file);
    co_return;
}

int main() {
    auto action = winrt::attach_cancellable(SaveDataAsync(L"output.txt", "Hello from CppWinC!"));
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
    std::uint32_t pid = GetCurrentProcessId();
    std::cout << "PID: " << pid << std::endl;
}

// WDK type conversions are available from Windows.Wdk.BASETSD
void convert_handles() {
    using namespace Windows::Wdk::BASETSD;
    void* handle = nullptr;
    std::int32_t value = HandleToLong(handle);
    void* restored = LongToHandle(value);
    (void)restored;
}
```

## External C++ code: headers, libs, DLLs (externals/)

Consuming custom C++ code, native C/C++ libraries, and external DLLs never requires editing generated files or passing extra flags — it is a plain folder convention under the projection folder (the `-output` folder, or the folder given to `-compile`):

```
externals/
├── include/ ...      every directory named `include` is added as /I to
│                     every module (.ixx) and .cpp compile (headers dropped
│                     directly in externals/ resolve too)
├── **/*.lib          every import/static library found is added to the
│                     exe, sys and dll links
└── **/*.dll          copied next to the final linked binaries
```

NuGet package folders can be dropped in verbatim — `build/native/include`, `lib/x64` and `runtimes/win-x64/native` all match the scan. No switches: the convention is discovered from the folder itself. `cppwinc` creates only the top-level `externals/` directory when absent; it does not create nested package directories, and it leaves the contents of an existing tree untouched.

What this gives you across all projection targets:

- **Zero-Config Header Resolution** — headers placed directly in `externals/` or within any `include/` subfolder are instantly discoverable via `#include` across all generated `.ixx` modules and `.cpp` implementation files.
- **Automatic Linker Ingestion** — every `.lib` file (import libraries, static libraries) discovered recursively within `externals/` is automatically appended to the linker command line for executable (`.exe`), kernel driver (`.sys`), and dynamic library (`.dll`) targets.
- **Runtime Binary Deployment** — every `.dll` file found anywhere under `externals/` is automatically copied alongside the final linked binaries in the output directory.

## Usage

```
cppwinc.exe [options...]

Options:
    -input <spec>        Windows metadata to include - ONE comma-separated list
                         (local, <path-to-winmd-or-folder>, 10.0.x). A
                         winuic-produced winmd here activates the WinUI3
                         pipeline automatically (default behavior); -winui
                         auto-adds WASDK metadata and finds its runtime DLLs
    -output <path>       Location of generated projection (also the default
                         landing spot for every other switch's output)
    -component [<path>]  Generate component templates and implementation
                         into <path> (default: the -output folder)
    -name <name>         Specify explicit name for component files
    -verbose             Show detailed progress information
    -include <list>      Namespace prefixes to include - ONE comma-separated
                         list or repeated switches (e.g.
                         -include Windows.Win32.Graphics,Windows.Win32.Storage)
    -exclude <list>      Namespace prefixes to exclude - same form as
                         -include; takes precedence over -include
    -base                Generate base module unconditionally
    -std [<path>]        Build the std named module into <path>
    -winui [exp] [debug] [<path>]
                         Auto-discover WASDK metadata + DLLs, build std,
                         compile all modules; optional 'exp' = latest
                         experimental winmds only (default: latest stable);
                         optional <path> receives ONLY the final linked
                         binaries (+ resources.pri, Assets, LICENSE, bootstrap DLL
                         beside the exe)
    -winui_no_compile [exp] [<path>]
                         Like -winui but skip compilation (generate only);
                         same optional 'exp' channel and <path> outpath
                         support (+ resources.pri, Assets, LICENSE, bootstrap DLL
                         beside the exe)
    -compile [debug] [<path>]
                         Compile all modules; optional 'debug' = unoptimized
                         debug build (/MDd /Od /Zi /D_DEBUG; default: release
                         /MD /O2 /DNDEBUG); optional <path> receives ONLY the
                         final linked binaries (+ link exe / sys / dll)
    -target <list>       Link targets for -compile (requires -compile):
                         comma-separated exe, sys, dll
    -help, -h, --help    Show detailed help with examples
    -module_include <list>
                         Only generate modules for the listed namespaces -
                         ONE comma-separated list or repeated switches
    -module_exclude <list>
                         Skip modules for the listed namespaces - same form
                         as -module_include; takes precedence over
                         -module_include

Where <spec> is a single comma-separated list of any of:
  path                Path to winmd file or recursively scanned folder
  local               Local %WinDir%\System32\WinMetadata folder
  10.0.12345.0        Specific version of the Windows SDK metadata
```

### Generate a projection

```bat
:: OS metadata plus a folder of extra winmd files (e.g. WASDK/WinUI3)
cppwinc.exe -input local,<path\to\winmd-folder> -base -output out

:: WinUI app - the winuic-produced winmd turns the WinUI3 pipeline on by
:: itself (default behavior): WASDK metadata + DLLs are discovered, views
:: and modules are generated, then everything is compiled and linked.
:: The winversec Windows.Win32.winmd is still required for the main.ixx
:: bootstrap.
cppwinc.exe -input MyApp.winmd,Windows.Win32.winmd

:: Same pipeline, but stop after generation (explicit generate-only mode)
cppwinc.exe -input MyApp.winmd,Windows.Win32.winmd -winui_no_compile

:: Multiple winmd files, comma-separated
cppwinc.exe -input Xaml.winmd,Foundation.winmd -base -output out
```

### Generate a Win32 projection

```bat
cppwinc.exe -input Windows.Win32.winmd -base -output out
```

### Generate a component with `-component`

`-component` emits the WinRT component templates and the matching
implementation sources (activation factory glue: `create_instance`,
`iface_cache`, `implements_type`, `default_interface`) for an app winmd:

```bat
:: component files into a dedicated folder (normalized like -output)
cppwinc.exe -input local,MyApp.winmd -component out\component -name MyApp -output out

:: flag without a path: component files land in the -output folder
:: (the default — same rule as every other switch's output)
cppwinc.exe -input local,MyApp.winmd -component -output out

:: -winui generates the component automatically (no -component needed);
:: the winversec Windows.Win32.winmd is required for the bootstrap
cppwinc.exe -input MyApp.winmd,Windows.Win32.winmd -winui -output .
```

How `-component` resolves its inputs and outputs:

- **Component source = the app `-input` winmd(s).** Every user-supplied
  winmd named in `-input` is an app/component source; system metadata
  (`local`, SDK contracts) and the generated `Windows.Win32` / `Windows.Wdk`
  projections never name the component. Position in `-input` does not
  matter — order is irrelevant.
- **`-name <name>`** sets the explicit component file name; without it the
  app winmd stem is used.
- **`-component [<path>]`** chooses the destination folder; without a path
  the component files go to the `-output` folder (the path is resolved
  against the launch directory exactly like `-output`/`-compile`).
- Component modules are ordinary `.ixx` modules: they compile with
  `-compile` (release, all cores) like the rest of the projection, and
  `-winui` always enables component generation (it also emits `main.ixx`
  with the WASDK bootstrap).


### Authoring a general WinRT component (`-component` for plain APIs)

`-component` is not WinUI-specific — use it to author any WinRT runtime
class, interface, struct, enum, or delegate (a class library that exposes
general WinRT APIs). Point it at your own component `.winmd` in `-input`
next to the OS metadata your types reference — order does not matter:

```bat
:: Author a general WinRT class library from its winmd.
:: -component emits the
:: projection for your types PLUS the implementation + activation templates.
:: -name names the component files (default: the winmd stem).
:: -compile builds everything (release, all cores) in the same run.
cppwinc.exe -input local,MyTypes.winmd -component out\component -name MyTypes -output out -compile

:: -component without a path: templates land in the -output folder
cppwinc.exe -input local,MyTypes.winmd -component -name MyTypes -output out
```

For a general WinRT API surface `-component` emits:

- **Projection modules** for every runtime class, interface, value type,
  enum, and delegate in your winmd — importable by consumers like any
  other `winrt.*` module.
- **Implementation templates** for the authoring side: the CRTP
  `implements<D, I...>` base, `default_interface`, `iface_cache`, and
  `implements_type` glue, plus the class skeleton with method stubs to
  fill in.
- **Activation-factory registration** so consumers can activate and call
  the class through `create_instance(class_name)` (the same helper the
  runtime helpers section documents), resolving the factory, default
  interface, and interface cache automatically.

Implement the generated method stubs, then build the projection modules
and the component modules together with `-compile` (release, all cores),
exactly like the rest of the projection. `-name <name>` overrides the
component file name, and `-component <path>` chooses where the templates
land (both default to the `-output` folder / the winmd stem).


### MIDL-authored components (`midlrt` + `-component`)

Define the component surface in IDL, compile it to a winmd with the
Windows SDK's MIDLRT compiler, then hand that winmd to `-component`
(anywhere in `-input` — the order of winmds does not matter):

```bat
:: 1. IDL -> component/authoring winmd (midlrt ships with the Windows SDK)
midlrt /winrt /metadata_dir "C:\Program Files (x86)\Windows Kits\10\References\10.0.26100.0" MyTypes.idl
::    -> MyTypes.winmd (activatable classes, factory interfaces, contracts)

:: 2. -component reads it like any other authoring winmd
cppwinc.exe -input local,MyTypes.winmd -component out\component -name MyTypes -output out -compile
```

The midlrt metadata drives the projection modules, implementation
templates, and activation-factory registration - same pipeline as a
winmd from any other authoring toolchain.

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

### Build with `-compile` (and `-target`)

`-compile` runs the built-in module compilation + link pass — the same
machinery `-winui` uses — without WASDK auto-discovery, for winrt / win32 /
wdk projections and any custom winmd input:

```bat
:: generate + compile + link in one shot
cppwinc.exe -input Windows.Win32+Wdk.winmd -output build\proj -compile

:: quick-test an existing projection folder (no -input, no regeneration)
cppwinc.exe -compile build\proj

:: only the final binaries land here (modules/objs stay with -output/source)

cppwinc.exe -input Windows.Win32.winmd -output out -compile out\bin

:: force link targets (comma-separated; requires -compile)
cppwinc.exe -compile build\proj -target exe,dll
```

What `-compile` guarantees:

- **Release by default, Debug with `debug`.** Modules and `.cpp` sources compile with
  `/MD /O2 /DNDEBUG`; the exe, sys and dll links all run with
  `/RELEASE /OPT:REF /OPT:ICF` (`/INCREMENTAL:NO`). There is no debug
  Passing `debug` (`-compile debug` or `-winui debug` / `-winui exp debug`) switches compilation to `/MDd /Od /Zi /D_DEBUG` and links with `/DEBUG:FULL`.
- **All cores in parallel.** The topological module build is leveled and
  compiles every ready module concurrently across `system_core_count()`
  threads (set `CPPWINC_JOBS=<n>` to override the thread count).
- **exe + sys + dll in ONE pass.** Link targets are auto-detected from the
  project sources, and every matching target links independently:

  | target | detected by | output |
  |--------|-------------|--------|
  | `<name>.exe` | `main.ixx` present (WinMain app) or a `.cpp` with `int main(` (console) | `<name>.exe` |
  | `<name>.sys` | any `.cpp` mentioning `DriverEntry` / `WDF_DRIVER_CONFIG` | `<name>.sys` |
  | `<name>.dll` | any `.cpp` mentioning `DllMain`, or a `.def` file (used as `/DEF:`) | `<name>.dll` |

  A folder carrying a driver source *and* a `main.ixx` app produces both
  `<name>.sys` and `<name>.exe` from the same run.
- **`-target exe,sys,dll` overrides auto-detection** (comma-separated,
  case-insensitive). It requires `-compile`; using it with `-winui`/
  `-winui_no_compile` or without any build switch is an error. A forced
  target whose source is missing is an error too (`-target sys` with no
  `DriverEntry`, `-target exe` with no `main.ixx`/`int main(`).
- **Output placement.** Intermediates (`.obj`, `.ifc`, logs, `.rsp`) always
  stay in `<project>\build\obj`. Final binaries go to, in priority order:
  the `-compile <path>` (or `-winui <path>`) folder — which then contains
  ONLY the final output — else the `-output` folder, else the project
  folder (quick-test default).
- **Mutually exclusive modes.** `-compile` cannot combine with `-winui`
  or `-winui_no_compile` (those already compile), and `-winui` with
  `-winui_no_compile`. Each combination fails fast with an error.
- **Quick-test caveat.** `-compile <folder>` compiles exactly what the
  folder contains: cross-module imports only resolve when the imported
  modules exist in the folder (generate the full projection, or list the
  whole closure with `-module_include`).

### One-shot build with `-winui`

The `-winui` flag handles everything in one command — auto-discovers
WASDK metadata and runtime DLLs from NuGet, builds the `std` module,
generates the projection, and compiles all modules in topological order
(release, all cores):

```bat
cppwinc.exe -input MyApp.winmd,Windows.Win32.winmd -winui -output .

:: optional path: ONLY the final binaries (+ resources.pri, Assets,
:: bootstrap DLL) land there; modules stay in -output
cppwinc.exe -input MyApp.winmd,Windows.Win32.winmd -winui out\bin -output out
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
   with 94K+ methods) into balanced chunks (`P1`, `P2`, ...).9. Compiles all modules using direct `cl.exe` invocation with auto-detected
MSVC toolchain, SDK, and WASDK include paths.

No hardcoded paths — works on any Windows machine with Visual Studio and NuGet.

#### App naming

The app name comes from `-name`, or from the input winmd stem; spaces are
hyphen-joined once (`WinUI Gallery` → `WinUI-Gallery`) and a `-name` value is
taken verbatim. That spelling names the artifacts — `<App>.exe`, `<App>.pri`,
the input winmd — and is retained in activation strings
(`L"WinUI-Gallery.App"`). C++ identifiers derived from it are underscored, as
the language requires (`WinUI_Gallery.App`, `winrt::WinUI_Gallery`).

#### Experimental channel (`exp`)

Both switches take an optional `exp` argument — `-winui exp` /
`-winui_no_compile exp` — restricting WASDK winmd discovery to experimental
packages (version directories tagged `-experimental*`, e.g.
`2.3.10-experimental`): the newest installed experimental version wins and
stable releases are skipped. Without `exp`, experimental directories are
skipped and the latest stable version of each package is picked. Same channel
semantics as winversec's `-include_wasdk [exp]`.

```bat
:: latest stable WASDK winmds (default)
cppwinc.exe -input MyApp.winmd,Windows.Win32.winmd -winui -output out

:: latest experimental WASDK winmds only
cppwinc.exe -input MyApp.winmd,Windows.Win32.winmd -winui exp -output out
```

#### Debug builds (debug)

Both -winui and -compile default to optimized **Release** builds (/MD /O2 /DNDEBUG compiler flags and /RELEASE /OPT:REF /OPT:ICF linker flags).

Passing the debug positional keyword enables unoptimized debug builds with full debug information and debug CRT symbols (/MDd /Od /Zi /D_DEBUG and /DEBUG:FULL linker flags):

```bat
:: Release WinUI build (default)
cppwinc.exe -input MyApp.winmd,Windows.Win32.winmd -winui -output out

:: Debug WinUI build
cppwinc.exe -input MyApp.winmd,Windows.Win32.winmd -winui debug -output out

:: Debug WinUI build with experimental WASDK packages
cppwinc.exe -input MyApp.winmd,Windows.Win32.winmd -winui exp debug -output out

:: Standalone compile in Debug mode
cppwinc.exe -compile debug -output out
```

#### System metadata and module selection

Both switches add system metadata automatically, so `-input local` is
unneeded: WASDK winmds from the NuGet cache, then the newest Windows SDK
`UnionMetadata` folder, falling back to `%WinDir%\System32\WinMetadata` (what
`local` resolves to) only when that folder is absent.

Emission is selective - only modules reachable from this run's view modules,
`main.ixx` and `winrt_base` are generated; unneeded `winrt.*` modules are
deleted afterwards. Win32/WDK modules resolve through their own namespace's
dependencies, keeping a `-winui` build small. `-winui_no_compile` yields the
same pruned set without compiling; a plain `-input`/`-compile` build is not
pruned at all.

> [!IMPORTANT]
> **A winversec-produced `Windows.Win32.winmd` in `-input` is required.**
> Both switches fail fast without one, since `main.ixx` projects the
> bootstrap's Win32 calls (`PACKAGE_VERSION`, `MddBootstrap*`,
> `LoadLibraryExW`) from metadata rather than Windows headers:
>
> ```bat
> winversec.exe -no_drivers -include_wasdk -output Windows.Win32.winmd
> cppwinc.exe -input winuic_out\MyApp.winmd,Windows.Win32.winmd -winui -output out
> ```
>
> Without these switches the file is projected like any other `-input`.

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
- **Shared `winui_base` module** — avoids duplicate prelude parsing across
  hundreds of view modules.
- **Safe delegate & component ABI marshaling** — `borrow_arg_in<T>` ensures
  borrowed runtime-class parameters (`sender`, `args`) initialize a clean
  `iface_cache`, and `style_lookup_th` / `apply_xaml_resources` handle
  `*Style` attributes and leading-whitespace XAML resource fragments cleanly.

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
cppwinc-compiler/
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
├── winuic_data.ixx          # WinUI 3 JSON, AST, catalog, stubs, expr mapping
├── winuic_template.ixx      # DataTemplates, React factories, runtime helpers
├── winuic_control.ixx       # Control emission engine and tree traversal
├── winuic_view.ixx          # View emission, BuildUI, code fixup post-pass
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


