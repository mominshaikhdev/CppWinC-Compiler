// async_data.ixx - module async_data.
// The async protocol and coroutine machinery for the projected base
// module (winrt_base.ixx): the shared completed-handler delegate, the
// IAsyncInfo vtable plumbing, blocking waits, and the co_await adapters.
// Moved out of strings_data.ixx so the async implementation lives in its
// own module; strings_data assembles base_module_ixx from the head, this
// chunk, and the tail.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;

#include <string_view>

export module async_data;

namespace cppwinc::strings
{
    // The async section of winrt_base.ixx.

    export inline constexpr std::string_view base_async_ixx = R"cppwinc(
    // coroutine and blocking-wait support
    //
    // The concrete async interfaces are dispatched through the generated typed
    // wrappers in the projection modules, which pass fully typed
    // completed-handler delegates at the ABI slots that match the SDK's
    // interface vtables:
    //   - IAsyncAction / IAsyncOperation<T>: 6 put_Completed, 7 get_Completed,
    //     8 GetResults;
    //   - the *WithProgress variants insert put_Progress 6 and get_Progress 7
    //     (8 put_Completed, 9 get_Completed, 10 GetResults).
    // IAsyncInfo is a separate interface reached only by QI; its vtable is
    // 6 get_Id, 7 get_Status, 8 get_ErrorCode, 9 Cancel, 10 Close (status_of
    // and error_of below). This base module provides only the shared state the
    // typed delegates drive: async_wait_state backs the blocking get() and
    // async_coroutine_state backs co_await. Each handler is a real, generically
    // typed delegate, because the OS put_Completed rejects bare handlers whose
    // IID does not match the concrete Async*CompletedHandler interface.

    namespace impl
    {
        export constexpr std::int32_t async_status_created = 0;
        export constexpr std::int32_t async_status_completed = 1;
        export constexpr std::int32_t async_status_canceled = 2;
        export constexpr std::int32_t async_status_error = 3;
        export constexpr std::int32_t async_status_started = 4;

        // Header-free: winrt_guid is ABI-identical to the OS GUID; APIs
        // come from the winversec-projected Com module (raw void* ABI).
        constexpr winrt_guid iid_async_info{ 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
        constexpr winrt_guid iid_agile_object{ 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };
        constexpr winrt_guid iid_marshal{ 0x00000019, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

        // IMarshal implementation backed by the free-threaded marshaler, so
        // that projected delegates can be dispatched from the OS's async
        // completion/thread pool apartments. Mirrors the official runtime's
        // delegate QI (IAgileObject + IMarshal) exactly.

        export struct delegate_marshaler
        {
            void* m_object = nullptr;
            void* m_marshal = nullptr;
            std::atomic<std::int32_t> m_refs{ 1 };

            static void* const* vtable() noexcept
            {
                static void* const instance[9] =
                {
                    reinterpret_cast<void*>(&delegate_marshaler::qi),
                    reinterpret_cast<void*>(&delegate_marshaler::add_ref),
                    reinterpret_cast<void*>(&delegate_marshaler::release),
                    reinterpret_cast<void*>(&delegate_marshaler::get_unmarshal_class),
                    reinterpret_cast<void*>(&delegate_marshaler::get_marshal_size_max),
                    reinterpret_cast<void*>(&delegate_marshaler::marshal_interface),
                    reinterpret_cast<void*>(&delegate_marshaler::unmarshal_interface),
                    reinterpret_cast<void*>(&delegate_marshaler::release_marshal_data),
                    reinterpret_cast<void*>(&delegate_marshaler::disconnect_object),
                };
                return instance;
            }

            explicit delegate_marshaler(void* object) noexcept : m_object(object)
            {
                winrt::IUnknown::addref_raw(m_object);
                namespace com_ns = Windows::Win32::Com;
                void* ftm = nullptr;
                if (com_ns::CoCreateFreeThreadedMarshaler(nullptr, &ftm) >= 0 && ftm)
                {
                    auto qi = reinterpret_cast<std::int32_t(__stdcall*)(void*, winrt::winrt_guid const&, void**)>(*reinterpret_cast<void* const* const*>(ftm));
                    void* imarshal = nullptr;
                    if (qi(ftm, iid_marshal, &imarshal) >= 0)
                    {
                        m_marshal = imarshal;
                    }
                    winrt::IUnknown::release_raw(ftm);
                }
            }

            ~delegate_marshaler()
            {
                if (m_marshal)
                {
                    winrt::IUnknown::release_raw(m_marshal);
                }
                winrt::IUnknown::release_raw(m_object);
            }

            static std::int32_t __stdcall qi(void* self, winrt_guid const& iid, void** result) noexcept
            {
                if (iid == iid_marshal)
                {
                    *result = self;
                    static_cast<delegate_marshaler*>(self)->m_refs.fetch_add(1, std::memory_order_relaxed);
                    return hr_ok;
                }
                auto* me = static_cast<delegate_marshaler*>(self);
                auto const* vt = *reinterpret_cast<void* const* const*>(me->m_object);
                auto obj_qi = reinterpret_cast<std::int32_t(__stdcall*)(void*, winrt_guid const&, void**)>(vt[0]);
                return obj_qi(me->m_object, iid, result);
            }

            static std::uint32_t __stdcall add_ref(void* self) noexcept
            {
                return static_cast<delegate_marshaler*>(self)->m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
            }

            static std::uint32_t __stdcall release(void* self) noexcept
            {
                auto* me = static_cast<delegate_marshaler*>(self);
                std::int32_t remaining = me->m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if (remaining == 0)
                {
                    delete me;
                }
                return remaining;
            }

            static std::int32_t __stdcall get_unmarshal_class(void* self, void* riid, void* pv, std::uint32_t dwDestContext, void* pvDestContext, std::uint32_t mshlflags, void* pCid) noexcept
            {
                auto* me = static_cast<delegate_marshaler*>(self);
                return me->m_marshal ? reinterpret_cast<std::int32_t(__stdcall*)(void*, void*, void*, std::uint32_t, void*, std::uint32_t, void*)>(
                    (*reinterpret_cast<void* const* const*>(me->m_marshal))[3])(me->m_marshal, riid, pv, dwDestContext, pvDestContext, mshlflags, pCid) : hr_out_of_memory;
            }

            static std::int32_t __stdcall get_marshal_size_max(void* self, void* riid, void* pv, std::uint32_t dwDestContext, void* pvDestContext, std::uint32_t mshlflags, void* pSize) noexcept
            {
                auto* me = static_cast<delegate_marshaler*>(self);
                return me->m_marshal ? reinterpret_cast<std::int32_t(__stdcall*)(void*, void*, void*, std::uint32_t, void*, std::uint32_t, void*)>(
                    (*reinterpret_cast<void* const* const*>(me->m_marshal))[4])(me->m_marshal, riid, pv, dwDestContext, pvDestContext, mshlflags, pSize) : hr_out_of_memory;
            }

            static std::int32_t __stdcall marshal_interface(void* self, void* pStm, void* riid, void* pv, std::uint32_t dwDestContext, void* pvDestContext, std::uint32_t mshlflags) noexcept
            {
                auto* me = static_cast<delegate_marshaler*>(self);
                return me->m_marshal ? reinterpret_cast<std::int32_t(__stdcall*)(void*, void*, void*, void*, std::uint32_t, void*, std::uint32_t)>(
                    (*reinterpret_cast<void* const* const*>(me->m_marshal))[5])(me->m_marshal, pStm, riid, pv, dwDestContext, pvDestContext, mshlflags) : hr_out_of_memory;
            }

            static std::int32_t __stdcall unmarshal_interface(void* self, void* pStm, void* riid, void** ppv) noexcept
            {
                auto* me = static_cast<delegate_marshaler*>(self);
                return me->m_marshal ? reinterpret_cast<std::int32_t(__stdcall*)(void*, void*, void*, void**)>(
                    (*reinterpret_cast<void* const* const*>(me->m_marshal))[6])(me->m_marshal, pStm, riid, ppv) : hr_out_of_memory;
            }

            static std::int32_t __stdcall release_marshal_data(void* self, void* pStm) noexcept
            {
                auto* me = static_cast<delegate_marshaler*>(self);
                return me->m_marshal ? reinterpret_cast<std::int32_t(__stdcall*)(void*, void*)>(
                    (*reinterpret_cast<void* const* const*>(me->m_marshal))[7])(me->m_marshal, pStm) : hr_out_of_memory;
            }

            static std::int32_t __stdcall disconnect_object(void* self, std::uint32_t dwReserved) noexcept
            {
                auto* me = static_cast<delegate_marshaler*>(self);
                return me->m_marshal ? reinterpret_cast<std::int32_t(__stdcall*)(void*, std::uint32_t)>(
                    (*reinterpret_cast<void* const* const*>(me->m_marshal))[8])(me->m_marshal, dwReserved) : hr_out_of_memory;
            }
        };

        export inline std::int32_t status_of(void* asyncInfo) noexcept
        {
            auto const* vt = *reinterpret_cast<void* const* const*>(asyncInfo);
            auto qi = reinterpret_cast<std::int32_t(__stdcall*)(void*, winrt_guid const&, void**)>(vt[0]);
            void* info = nullptr;
            if (qi(asyncInfo, iid_async_info, &info) < 0)
            {
                return async_status_error;
            }
            auto const* ivt = *reinterpret_cast<void* const* const*>(info);
            auto get_status = reinterpret_cast<std::int32_t(__stdcall*)(void*, void**)>(ivt[7]);
            std::int32_t status = 0;
            get_status(info, reinterpret_cast<void**>(&status));
            reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(ivt[2])(info);
            return status;
        }

        export inline std::int32_t error_of(void* asyncInfo) noexcept
        {
            if (!asyncInfo)
            {
                return hr_error;
            }
            auto const* vt = *reinterpret_cast<void* const* const*>(asyncInfo);
            auto qi = reinterpret_cast<std::int32_t(__stdcall*)(void*, winrt_guid const&, void**)>(vt[0]);
            void* info = nullptr;
            if (qi(asyncInfo, iid_async_info, &info) < 0)
            {
                return hr_error;
            }
            auto const* ivt = *reinterpret_cast<void* const* const*>(info);
            auto get_error = reinterpret_cast<std::int32_t(__stdcall*)(void*, void**)>(ivt[8]);
            std::int32_t error = hr_error;
            std::int32_t hr = get_error(info, reinterpret_cast<void**>(&error));
            reinterpret_cast<std::uint32_t(__stdcall*)(void*)>(ivt[2])(info);
            return hr < 0 ? hr : error;
        }

        // shared COM shell for the delegate implementations. The vtable and
        // the reference counting live here once; each generated delegate
        // impl supplies its own Invoke and its concrete interface IID, so
        // the OS put_Completed can validate the handler's identity.

        export template <typename D>
        struct com_handler_base
        {
            // The vtable is built lazily on first use so that the derived
            // type is complete when its Invoke is addressed. WinRT delegate
            // ABIs derive from IUnknown (see cppwinrt's write_delegate_abi:
            // "struct type : unknown_abi"), so Invoke lives at slot 3.
            static void* const* vtable() noexcept
            {
                static void* const instance[4] =
                {
                    reinterpret_cast<void*>(&com_handler_base::qi),
                    reinterpret_cast<void*>(&com_handler_base::add_ref),
                    reinterpret_cast<void*>(&com_handler_base::release),
                    reinterpret_cast<void*>(&D::invoke),
                };
                return instance;
            }

            void* const* m_vtable = vtable();
            std::atomic<std::int32_t> m_refs{ 1 };

            com_handler_base() noexcept = default;

            // Generated delegate impls override this with the delegate's
            // interface iid; the default never matches, so plain handlers
            // answer only IUnknown/IInspectable.
            static winrt::winrt_guid const& iid() noexcept { return null_guid; }

            static std::int32_t __stdcall qi(void* self, winrt_guid const& iid, void** result) noexcept
            {
                // WinRT delegates implement IUnknown only, but the OS requires
                // them to be agile (or marshalable): async completions and
                // thread pool callbacks probe for IAgileObject first and then
                // IMarshal, mirroring the official runtime's delegate QI.
                if (iid == winrt::iid_iunknown || iid == iid_agile_object || iid == winrt_guid(D::iid()))
                {
                    *result = self;
                    static_cast<D*>(self)->m_refs.fetch_add(1, std::memory_order_relaxed);
                    return hr_ok;
                }
                if (iid == iid_marshal)
                {
                    *result = new (std::nothrow) delegate_marshaler(self);
                    return *result ? hr_ok : hr_out_of_memory;
                }
                *result = nullptr;
                return hr_no_interface;
            }

            static std::uint32_t __stdcall add_ref(void* self) noexcept
            {
                return static_cast<D*>(self)->m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
            }

            static std::uint32_t __stdcall release(void* self) noexcept
            {
                auto* me = static_cast<D*>(self);
                std::int32_t remaining = me->m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if (remaining == 0)
                {
                    delete me;
                }
                return remaining;
            }
        };

        // blocking get(): an event plus the status the completion handler
        // reports. The generated get() shares this with the typed delegate
        // through shared_ptr, so it cannot be destroyed while the delegate
        // (and through it the async object) may still reference it.

        export struct async_wait_state
        {
            // std::binary_semaphore keeps this header-free and independent of
            // the Win32 sync module (not part of every project's closure).
            std::binary_semaphore m_event{ 0 };
            std::int32_t m_status = async_status_created;

            void invoke(std::int32_t status) noexcept
            {
                m_status = status;
                m_event.release();
            }

            std::int32_t wait()
            {
                m_event.acquire();
                return m_status;
            }
        };

        // co_await support: the atomic gate, the coroutine handle, and the
        // error slot, driven by the typed completed handler through
        // complete_status. The gate protocol guarantees that completion
        // before await_suspend runs defers to it and completion after it
        // runs resumes directly, so a synchronous fire during put_Completed
        // cannot corrupt or strand the coroutine: whoever completes first
        // either resumes directly or defers to await_suspend, and
        // await_suspend returns the exchange result.

        export struct async_coroutine_state
        {
            std::atomic<std::int32_t> m_suspending{ 1 };
            std::atomic<void*> m_handle{ nullptr };
            std::int32_t* m_error = nullptr;

            explicit async_coroutine_state(std::int32_t* error) noexcept : m_error(error) {}

            void prepare(std::coroutine_handle<void> handle) noexcept
            {
                m_handle.store(handle.address(), std::memory_order_release);
            }

            // records the outcome, then completes the gate exactly like the
            // former shared delegate shell; see the gate protocol above.
            void complete_status(std::int32_t status, void* asyncInfo) noexcept
            {
                *m_error = status == async_status_canceled ? hr_canceled
                    : status == async_status_error ? error_of(asyncInfo) : hr_ok;
                if (m_suspending.exchange(false, std::memory_order_release))
                {
                    m_handle = nullptr; // resumption deferred to await_suspend
                }
                else if (auto address = m_handle.exchange(nullptr, std::memory_order_acquire))
                {
                    std::coroutine_handle<void>::from_address(address).resume();
                }
            }

            // await_suspend asks whether to suspend; the exchange happens
            // after the completion handler is registered, so it returns true
            // only when the completion cannot have been missed.
            bool suspend() noexcept
            {
                return m_suspending.exchange(false, std::memory_order_acquire);
            }

            // completes the gate with a caller-supplied error code (used by
            // the component-side awaiters, which know the concrete error)
            void complete(std::int32_t error) noexcept
            {
                *m_error = error;
                if (m_suspending.exchange(false, std::memory_order_release))
                {
                    m_handle = nullptr; // resumption deferred to await_suspend
                }
                else if (auto address = m_handle.exchange(nullptr, std::memory_order_acquire))
                {
                    std::coroutine_handle<void>::from_address(address).resume();
                }
            }
        };

        export inline void check_sta_blocking_wait()
        {
            // Header-free: APTTYPE is an int enum — 0=STA, 3=MAINSTA,
            // 1=MTA (matches the SDK's objidl.h). CoGetApartmentType comes
            // from the winversec-projected Com module (raw void* ABI).
            // APTTYPE_MTA = 1
            std::int32_t apt_type = 1;
            std::int32_t apt_qualifier = 0;
            namespace com_ns = Windows::Win32::Com;
            // STA = 0, MAINSTA = 3
            if (com_ns::CoGetApartmentType(reinterpret_cast<void**>(&apt_type),
                    reinterpret_cast<void**>(&apt_qualifier)) >= 0
                && (apt_type == 0 || apt_type == 3))
            {
                throw hresult_error(hr_illegal_method_call, L"A blocking wait on a single-threaded apartment would deadlock.");
            }
        }

        // Component-side coroutine return machinery (cancellable_promise).
        //
        // Self-contained COM implementations of IAsyncInfo + IAsyncAction /
        // IAsyncOperation<T>, driven by a coroutine promise, so component
        // authors can write cancellable coroutines that expose a real async
        // object to consumers. The projected async wrappers live in the
        // projection modules and cannot be named here, so the vtables are
        // hand-rolled to the exact slots the projection dispatches:
        //   IAsyncInfo      6 get_Id, 7 get_Status, 8 get_ErrorCode,
        //                   9 Cancel, 10 Close;
        //   IAsyncAction / IAsyncOperation<T>
        //                   6 put_Completed, 7 get_Completed, 8 GetResults.
        // The promise and the return object each hold a reference to the
        // shared state, so the operation stays alive while the coroutine is
        // suspended AND while the consumer holds it. Cancellation is an
        // internal shared source (cancellation_token / get_cancellation_token)
        // observed by the coroutine, and also drives the operation's status.

        // IAsyncInfo / IAsyncAction / IAsyncOperation pattern GUIDs
        export constexpr winrt_guid guid_iasync_info{ 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
        export constexpr winrt_guid guid_iasync_action{ 0x5a648006, 0x843a, 0x4da9, { 0x86, 0x5b, 0x9d, 0x26, 0xe5, 0xdf, 0xad, 0x7b } };
        export constexpr winrt_guid guid_iasync_operation{ 0x9fc2b0bb, 0xe446, 0x44e2, { 0xaa, 0x61, 0x9c, 0xab, 0x8f, 0x63, 0x6a, 0xf2 } };
        export constexpr winrt_guid guid_async_action_completed_handler{ 0xa4ed5c81, 0x76c9, 0x40bd, { 0x8b, 0xe6, 0xb1, 0xd9, 0x0f, 0xb2, 0x0a, 0xe7 } };
        export constexpr winrt_guid guid_async_operation_completed_handler{ 0xfcdcf02c, 0xe5d8, 0x4478, { 0x91, 0x5a, 0x4d, 0x90, 0xb7, 0x4b, 0x83, 0xa5 } };

        // shared cancellation state: a single atomic flag plus an optional
        // callback (register_callback runs it immediately if already cancelled)
        export struct cancellation_source
        {
            std::atomic<bool> m_cancelled{ false };
            std::mutex m_mutex;
            std::function<void()> m_callback;

            bool is_cancelled() const noexcept
            {
                return m_cancelled.load(std::memory_order_acquire);
            }

            void register_callback(std::function<void()> const& callback)
            {
                if (!callback)
                {
                    return;
                }
                std::lock_guard lock(m_mutex);
                if (m_cancelled.load(std::memory_order_relaxed))
                {
                    callback();
                }
                else
                {
                    m_callback = callback;
                }
            }

            void cancel() noexcept
            {
                bool expected = false;
                if (m_cancelled.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                {
                    std::function<void()> callback;
                    {
                        std::lock_guard lock(m_mutex);
                        callback = std::move(m_callback);
                    }
                    if (callback)
                    {
                        callback();
                    }
                }
            }
        };

        export struct cancellation_token
        {
            std::shared_ptr<cancellation_source> m_source;

            bool is_cancelled() const noexcept
            {
                return m_source && m_source->is_cancelled();
            }

            void on_cancel(std::function<void()> const& callback) const
            {
                if (m_source)
                {
                    m_source->register_callback(callback);
                }
            }

            void cancel() const noexcept
            {
                if (m_source)
                {
                    m_source->cancel();
                }
            }

            explicit operator bool() const noexcept
            {
                return m_source != nullptr;
            }
        };

        // co_await winrt::impl::get_cancellation_token() — pulls the token
        // from the current coroutine's promise (which is robust across
        // resumptions on other threads); empty outside a cancellable
        // coroutine.
        export struct get_cancellation_token_awaiter
        {
            cancellation_token m_token;

            bool await_ready() const noexcept { return false; }

            template <typename Promise>
            bool await_suspend(std::coroutine_handle<Promise> handle) noexcept
            {
                if constexpr (requires { handle.promise().m_token; })
                {
                    m_token = handle.promise().m_token;
                }
                return false;
            }

            cancellation_token await_resume() const noexcept { return m_token; }
        };

        export inline get_cancellation_token_awaiter get_cancellation_token() noexcept
        {
            return {};
        }

        export template <typename T>
        struct cancellable_state;

        export template <typename T>
        struct cancellable_promise;

        // completed-handler delegate used by the component awaiters: fires
        // the shared async_coroutine_state gate with the concrete error
        export struct cancellable_wait_handler : com_handler_base<cancellable_wait_handler>
        {
            cancellable_state<void>* m_state = nullptr;
            std::shared_ptr<async_coroutine_state> m_wait;

            static winrt::winrt_guid const& iid() noexcept
            {
                static winrt_guid const value = guid_async_action_completed_handler;
                return value;
            }

            static std::int32_t __stdcall invoke(void* self, void* sender, std::int32_t status) noexcept;
        };

        // the shared COM state of one cancellable operation
        export template <typename T>
        struct cancellable_state
        {
            std::atomic<std::int32_t> m_refs{ 1 };
            std::atomic<std::int32_t> m_status{ async_status_created };
            std::atomic<std::int32_t> m_error{ hr_ok };
            std::uint32_t m_id = 0;
            std::mutex m_mutex;
            void* m_completed = nullptr; // completed handler (owned)
            std::shared_ptr<cancellation_source> m_cancel = std::make_shared<cancellation_source>();
            // void state (IAsyncAction) carries no result; the conditional
            // keeps the member declaration valid for both shapes
            std::conditional_t<std::is_void_v<T>, std::monostate, T> m_result{};

            struct info_iface : inspectable_iface_base<cancellable_state<T>>
            {
                using inspectable_iface_base<cancellable_state<T>>::from_self;

                static void* const* vtable() noexcept
                {
                    static void* const v[11] =
                    {
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::qi),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::add_ref),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::release),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::get_iids),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::get_class_name),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::get_trust),
                        reinterpret_cast<void*>(&get_id),
                        reinterpret_cast<void*>(&get_status),
                        reinterpret_cast<void*>(&get_error_code),
                        reinterpret_cast<void*>(&cancel),
                        reinterpret_cast<void*>(&close),
                    };
                    return v;
                }

                static std::int32_t __stdcall get_id(void* self, std::uint32_t* value) noexcept
                {
                    *value = from_self(self)->m_owner->m_id;
                    return hr_ok;
                }

                static std::int32_t __stdcall get_status(void* self, std::int32_t* value) noexcept
                {
                    *value = from_self(self)->m_owner->m_status.load(std::memory_order_acquire);
                    return hr_ok;
                }

                static std::int32_t __stdcall get_error_code(void* self, void** value) noexcept
                {
                    auto* owner = from_self(self)->m_owner;
                    // IAsyncInfo::ErrorCode's ABI out-param is a plain
                    // HResult value (official ABI: get_ErrorCode(winrt::hresult*)),
                    // NOT a boxed IInspectable. Writing a pointer here would
                    // both overflow the caller's 4-byte HResult and make
                    // error_of read the pointer's low bits as the error.
                    *reinterpret_cast<std::int32_t*>(value) = owner->m_error.load(std::memory_order_acquire);
                    return hr_ok;
                }

                static std::int32_t __stdcall cancel(void* self) noexcept
                {
                    auto* owner = from_self(self)->m_owner;
                    owner->cancel_operation();
                    return hr_ok;
                }

                static std::int32_t __stdcall close(void* self) noexcept
                {
                    (void)self;
                    return hr_ok;
                }
            };

            // IAsyncAction (T = void): GetResults has no out parameter
            struct action_iface : inspectable_iface_base<cancellable_state<T>>
            {
                using inspectable_iface_base<cancellable_state<T>>::from_self;

                static void* const* vtable() noexcept
                {
                    static void* const v[9] =
                    {
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::qi),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::add_ref),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::release),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::get_iids),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::get_class_name),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::get_trust),
                        reinterpret_cast<void*>(&put_completed),
                        reinterpret_cast<void*>(&get_completed),
                        reinterpret_cast<void*>(&get_results),
                    };
                    return v;
                }

                static std::int32_t __stdcall put_completed(void* self, void* handler) noexcept
                {
                    auto* owner = from_self(self)->m_owner;
                    try
                    {
                        owner->put_completed(handler);
                        return hr_ok;
                    }
                    catch (...)
                    {
                        return catch_to_hresult();
                    }
                }

                static std::int32_t __stdcall get_completed(void* self, void** result) noexcept
                {
                    auto* owner = from_self(self)->m_owner;
                    std::lock_guard lock(owner->m_mutex);
                    *result = owner->m_completed;
                    if (*result)
                    {
                        winrt::IUnknown::addref_raw(*result);
                    }
                    return hr_ok;
                }

                static std::int32_t __stdcall get_results(void* self) noexcept
                {
                    (void)self;
                    return hr_ok;
                }
            };

            // IAsyncOperation<T> (T != void): GetResults returns the value
            struct operation_iface : inspectable_iface_base<cancellable_state<T>>
            {
                using inspectable_iface_base<cancellable_state<T>>::from_self;

                static void* const* vtable() noexcept
                {
                    static void* const v[9] =
                    {
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::qi),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::add_ref),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::release),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::get_iids),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::get_class_name),
                        reinterpret_cast<void*>(&inspectable_iface_base<cancellable_state<T>>::get_trust),
                        reinterpret_cast<void*>(&put_completed),
                        reinterpret_cast<void*>(&get_completed),
                        reinterpret_cast<void*>(&get_results),
                    };
                    return v;
                }

                static std::int32_t __stdcall put_completed(void* self, void* handler) noexcept
                {
                    auto* owner = from_self(self)->m_owner;
                    try
                    {
                        owner->put_completed(handler);
                        return hr_ok;
                    }
                    catch (...)
                    {
                        return catch_to_hresult();
                    }
                }

                static std::int32_t __stdcall get_completed(void* self, void** result) noexcept
                {
                    auto* owner = from_self(self)->m_owner;
                    std::lock_guard lock(owner->m_mutex);
                    *result = owner->m_completed;
                    if (*result)
                    {
                        winrt::IUnknown::addref_raw(*result);
                    }
                    return hr_ok;
                }

                // T is never void here (the void shape uses action_iface); the
                // conditional keeps the declaration valid when the void state
                // instantiates its member classes eagerly
                static std::int32_t __stdcall get_results(void* self, put_abi_t<std::conditional_t<std::is_void_v<T>, std::int32_t, T>> out) noexcept
                {
                    if constexpr (std::is_void_v<T>)
                    {
                        // unreachable: the void shape uses action_iface
                        return hr_ok;
                    }
                    else
                    {
                        auto* owner = from_self(self)->m_owner;
                        try
                        {
                            if (owner->m_status.load(std::memory_order_acquire) != async_status_completed)
                            {
                                return hr_error;
                            }
                            collection_element<T>::to_abi(out, owner->m_result);
                            return hr_ok;
                        }
                        catch (...)
                        {
                            return catch_to_hresult();
                        }
                    }
                }
            };

            info_iface m_info;   // offset 0: the state's address is a valid
                                 // IAsyncInfo interface pointer
            action_iface m_action;
            operation_iface m_operation;

            cancellable_state()
            {
                static std::atomic<std::uint32_t> next_id{ 1 };
                m_id = next_id.fetch_add(1, std::memory_order_relaxed);
                m_info.m_vtable = info_iface::vtable();
                m_info.m_owner = this;
                m_action.m_vtable = action_iface::vtable();
                m_action.m_owner = this;
                m_operation.m_vtable = operation_iface::vtable();
                m_operation.m_owner = this;
            }

            void* get_abi() noexcept
            {
                if constexpr (std::is_void_v<T>)
                {
                    return &m_action;
                }
                else
                {
                    return &m_operation;
                }
            }

            std::int32_t status() const noexcept { return m_status.load(std::memory_order_acquire); }
            std::int32_t error() const noexcept { return m_error.load(std::memory_order_acquire); }
            bool is_done() const noexcept
            {
                std::int32_t s = status();
                return s != async_status_created && s != async_status_started;
            }

            // the completed handler must be a real delegate object (the OS
            // and the projection validate its IID); we take ownership of a
            // reference
            void put_completed(void* handler)
            {
                if (!handler)
                {
                    return;
                }
                winrt::IUnknown::addref_raw(handler);
                void* previous = nullptr;
                {
                    std::lock_guard lock(m_mutex);
                    previous = m_completed;
                    m_completed = handler;
                }
                if (previous)
                {
                    winrt::IUnknown::release_raw(previous);
                }
                // an already-completed operation must fire the handler now
                if (is_done())
                {
                    fire_completed();
                }
            }

            // never instantiated for the void shape; the conditional keeps
            // the parameter declaration valid
            void set_result(std::conditional_t<std::is_void_v<T>, std::int32_t, T> value)
            {
                m_result = std::move(value);
            }

            void set_error(std::int32_t error) noexcept
            {
                m_error.store(error, std::memory_order_release);
            }

            // called by the promise when the coroutine body ends
            void complete() noexcept
            {
                std::int32_t status = m_cancel->is_cancelled() ? async_status_canceled
                    : m_error.load(std::memory_order_acquire) != hr_ok ? async_status_error
                    : async_status_completed;
                if (status == async_status_canceled)
                {
                    m_error.store(hr_canceled, std::memory_order_release);
                }
                complete_from(status, m_error.load(std::memory_order_acquire));
            }

            // completes with an explicit status/error (used by the
            // when_all/when_any aggregators and by Cancel)
            void complete_from(std::int32_t status, std::int32_t error) noexcept
            {
                m_status.store(status, std::memory_order_release);
                m_error.store(error, std::memory_order_release);
                fire_completed();
            }

            void cancel_operation() noexcept
            {
                if (m_cancel->is_cancelled())
                {
                    return;
                }
                m_cancel->cancel();
                if (!is_done())
                {
                    m_error.store(hr_canceled, std::memory_order_release);
                    complete_from(async_status_canceled, hr_canceled);
                }
            }

            void fire_completed() noexcept
            {
                void* handler = nullptr;
                {
                    std::lock_guard lock(m_mutex);
                    handler = m_completed;
                    m_completed = nullptr;
                }
                if (!handler)
                {
                    return;
                }
                auto* vt = *reinterpret_cast<void* const**>(handler);
                auto invoke = reinterpret_cast<std::int32_t(__stdcall*)(void*, void*, std::int32_t)>(vt[3]);
                invoke(handler, get_abi(), m_status.load(std::memory_order_acquire));
                winrt::IUnknown::release_raw(handler);
            }

            std::uint32_t add_ref() noexcept { return m_refs.fetch_add(1, std::memory_order_relaxed) + 1; }

            std::uint32_t release() noexcept
            {
                std::uint32_t remaining = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if (remaining == 0)
                {
                    delete this;
                }
                return remaining;
            }

            std::int32_t query_interface(winrt::winrt_guid const& iid, void** result) noexcept
            {
                if (iid == winrt_guid::from(winrt::iid_iunknown) || iid == winrt_guid::from(winrt::iid_iinspectable) || iid == guid_iasync_info)
                {
                    *result = &m_info;
                    add_ref();
                    return hr_ok;
                }
                if constexpr (std::is_void_v<T>)
                {
                    if (iid == guid_iasync_action)
                    {
                        *result = &m_action;
                        add_ref();
                        return hr_ok;
                    }
                }
                else
                {
                    static winrt_guid const operation_iid = make_generic_iid(guid_iasync_operation, generic_arg_of<T>);
                    if (iid == operation_iid)
                    {
                        *result = &m_operation;
                        add_ref();
                        return hr_ok;
                    }
                }
                *result = nullptr;
                return hr_no_interface;
            }

            std::int32_t get_iids(std::uint32_t* count, winrt_guid** iids) noexcept
            {
                if constexpr (std::is_void_v<T>)
                {
                    static winrt_guid const list[] =
                    {
                        winrt_guid::from(winrt::iid_iunknown),
                        winrt_guid::from(winrt::iid_iinspectable),
                        guid_iasync_info,
                        guid_iasync_action,
                    };
                    *count = 4;
                    auto* result = new winrt_guid[4];
                    std::copy(std::begin(list), std::end(list), result);
                    *iids = result;
                }
                else
                {
                    static winrt_guid const operation_iid = make_generic_iid(guid_iasync_operation, generic_arg_of<T>);
                    static winrt_guid const list[] =
                    {
                        winrt_guid::from(winrt::iid_iunknown),
                        winrt_guid::from(winrt::iid_iinspectable),
                        guid_iasync_info,
                        operation_iid,
                    };
                    *count = 4;
                    auto* result = new winrt_guid[4];
                    std::copy(std::begin(list), std::end(list), result);
                    *iids = result;
                }
                return hr_ok;
            }

            std::int32_t get_class_name(void** name) noexcept
            {
                *name = nullptr;
                return hr_ok;
            }

            std::int32_t get_trust(std::int32_t* trust) noexcept
            {
                *trust = 0;
                return hr_ok;
            }
        };

        // defined after cancellable_state is complete
        export inline std::int32_t cancellable_wait_handler::invoke(void* self, void*, std::int32_t status) noexcept
        {
            auto* me = static_cast<cancellable_wait_handler*>(self);
            std::int32_t error = hr_ok;
            if (status == async_status_canceled)
            {
                error = hr_canceled;
            }
            else if (status == async_status_error)
            {
                error = me->m_state ? me->m_state->error() : hr_error;
            }
            me->m_wait->complete(error);
            return 0;
        }

        // forward: the combinators complete a cancellable_state<void>
        export inline void* make_when_all(void* const* actions, std::size_t count);
        export inline void* make_when_any(void* const* actions, std::size_t count);

        // completed-handler that observes one input action of a combinator
        export struct combinator_handler : com_handler_base<combinator_handler>
        {
            cancellable_state<void>* m_aggregator = nullptr;
            std::shared_ptr<std::atomic<std::int32_t>> m_remaining;
            std::shared_ptr<std::atomic<std::int32_t>> m_won;
            std::shared_ptr<std::atomic<std::int32_t>> m_error;
            // gates the aggregator release and the result reporting so that
            // exactly one of invoke / observe_action's failure path runs
            // them, even if the OS registers and fires the handler despite
            // put_Completed reporting an error (a completion race)
            std::atomic<bool> m_fired{ false };
            bool m_all = true;

            combinator_handler(cancellable_state<void>* aggregator,
                std::shared_ptr<std::atomic<std::int32_t>> remaining,
                std::shared_ptr<std::atomic<std::int32_t>> won,
                std::shared_ptr<std::atomic<std::int32_t>> error,
                bool all)
                : m_aggregator(aggregator), m_remaining(std::move(remaining)),
                  m_won(std::move(won)), m_error(std::move(error)), m_all(all)
            {
            }

            static winrt::winrt_guid const& iid() noexcept
            {
                static winrt_guid const value = guid_async_action_completed_handler;
                return value;
            }

            static std::int32_t __stdcall invoke(void* self, void* sender, std::int32_t status) noexcept
            {
                auto* me = static_cast<combinator_handler*>(self);
                if (me->m_fired.exchange(true, std::memory_order_acq_rel))
                {
                    return 0; // the observe_action failure path already ran
                }
                if (status == async_status_error)
                {
                    me->m_error->store(error_of(sender), std::memory_order_relaxed);
                }
                else if (status == async_status_canceled)
                {
                    me->m_error->store(hr_canceled, std::memory_order_relaxed);
                }
                if (me->m_all)
                {
                    if (me->m_remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
                    {
                        std::int32_t error = me->m_error->load(std::memory_order_relaxed);
                        me->m_aggregator->complete_from(error == hr_ok ? async_status_completed : async_status_error, error);
                    }
                }
                else
                {
                    int expected = 0;
                    if (me->m_won->compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
                    {
                        std::int32_t error = me->m_error->load(std::memory_order_relaxed);
                        me->m_aggregator->complete_from(
                            error == hr_ok ? async_status_completed
                            : error == hr_canceled ? async_status_canceled : async_status_error,
                            error);
                    }
                }
                // the aggregator reference taken when the handler was created
                // is released here, synchronously, exactly once (m_fired
                // guarantees invoke runs the bookkeeping at most once). The
                // release must not wait for the handler destructor: that runs
                // asynchronously after the OS drops the handler, racing the
                // wrapper's own release of the aggregator (use-after-free).
                me->m_aggregator->release();
                return 0;
            }
        };

        // registers a completed handler on an action (IAsyncAction vtable
        // slot 6) that reports into the combinator
        export inline void observe_action(void* action, combinator_handler* handler)
        {
            auto const* vt = *reinterpret_cast<void* const* const*>(action);
            auto fn = reinterpret_cast<std::int32_t(__stdcall*)(void*, void*)>(vt[6]);
            std::int32_t hr = fn(action, handler);
            if (hr < 0)
            {
                // put_Completed failed (e.g. the action already completed).
                // Normally the handler never fires, so report the failure and
                // release the aggregator claim right here. If the OS raced
                // and registered the handler anyway (it will fire later), the
                // m_fired gate lets whichever runs first do the work and the
                // other no-ops, so the aggregator is released exactly once.
                if (handler->m_fired.exchange(true, std::memory_order_acq_rel))
                {
                    // invoke already ran (synchronous fire during
                    // put_Completed): it reported the result and released the
                    // aggregator; just drop our handler reference
                    com_handler_base<combinator_handler>::release(handler);
                    return;
                }
                handler->m_error->store(hr, std::memory_order_relaxed);
                if (handler->m_all && handler->m_remaining->fetch_sub(1, std::memory_order_acq_rel) == 1)
                {
                    handler->m_aggregator->complete_from(async_status_error, hr);
                }
                else if (!handler->m_all)
                {
                    int expected = 0;
                    if (handler->m_won->compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
                    {
                        handler->m_aggregator->complete_from(async_status_error, hr);
                    }
                }
                // the OS never fired and never will: release the aggregator
                // claim and drop our handler reference (refcount-aware: if
                // the OS also took one during the raced registration, the
                // handler stays alive until the OS drops it)
                handler->m_aggregator->release();
                com_handler_base<combinator_handler>::release(handler);
                return;
            }
            // put_Completed succeeded: the OS owns a reference (it AddRefs
            // what it registers, verified by probe) and will fire the
            // handler; drop ours. invoke releases the aggregator when it
            // fires.
            com_handler_base<combinator_handler>::release(handler);
        }

        export inline void* make_when_all(void* const* actions, std::size_t count)
        {
            auto* aggregator = new cancellable_state<void>();
            if (count == 0)
            {
                aggregator->complete_from(async_status_completed, hr_ok);
                return aggregator->get_abi();
            }
            auto remaining = std::make_shared<std::atomic<std::int32_t>>((std::int32_t)count);
            auto won = std::make_shared<std::atomic<std::int32_t>>(0);
            auto error = std::make_shared<std::atomic<std::int32_t>>(hr_ok);
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!actions[i])
                {
                    continue;
                }
                aggregator->add_ref(); // held until the handler fires
                auto* handler = new combinator_handler(aggregator, remaining, won, error, true);
                observe_action(actions[i], handler);
            }
            return aggregator->get_abi();
        }

        export inline void* make_when_any(void* const* actions, std::size_t count)
        {
            auto* aggregator = new cancellable_state<void>();
            if (count == 0)
            {
                aggregator->complete_from(async_status_error, hr_invalid_argument);
                return aggregator->get_abi();
            }
            auto remaining = std::make_shared<std::atomic<std::int32_t>>((std::int32_t)count);
            auto won = std::make_shared<std::atomic<std::int32_t>>(0);
            auto error = std::make_shared<std::atomic<std::int32_t>>(hr_ok);
            for (std::size_t i = 0; i < count; ++i)
            {
                if (!actions[i])
                {
                    continue;
                }
                aggregator->add_ref(); // held until the handler fires
                auto* handler = new combinator_handler(aggregator, remaining, won, error, false);
                observe_action(actions[i], handler);
            }
            return aggregator->get_abi();
        }

        // the awaiter that lets one cancellable operation be co_await'ed by
        // another coroutine
        export template <typename T>
        struct cancellable_awaiter
        {
            cancellable_state<T>* m_state = nullptr;
            std::shared_ptr<async_coroutine_state> m_wait;
            std::int32_t m_error = hr_ok;

            explicit cancellable_awaiter(cancellable_state<T>* state) noexcept : m_state(state)
            {
                // hold a reference for the whole await (covers both the
                // already-done and the suspended paths)
                if (m_state)
                {
                    m_state->add_ref();
                }
            }

            cancellable_awaiter(cancellable_awaiter const&) = delete;

            cancellable_awaiter(cancellable_awaiter&& other) noexcept
                : m_state(other.m_state), m_wait(std::move(other.m_wait)), m_error(other.m_error)
            {
                other.m_state = nullptr;
            }

            ~cancellable_awaiter()
            {
                if (m_state)
                {
                    m_state->release();
                }
            }

            bool await_ready() const noexcept
            {
                return m_state && m_state->is_done();
            }

            bool await_suspend(std::coroutine_handle<void> handle)
            {
                m_wait = std::make_shared<async_coroutine_state>(&m_error);
                m_wait->prepare(handle);
                auto* handler = new cancellable_wait_handler;
                handler->m_state = reinterpret_cast<cancellable_state<void>*>(m_state);
                handler->m_wait = m_wait;
                m_state->put_completed(handler);
                return m_wait->suspend();
            }

            T await_resume()
            {
                if (m_error != hr_ok)
                {
                    throw hresult_error(m_error);
                }
                if constexpr (std::is_void_v<T>)
                {
                    return;
                }
                else
                {
                    return std::move(m_state->m_result);
                }
            }
        };

        // the coroutine return objects. Each holds a reference to the shared
        // state, so the operation outlives the caller's expression.
        export template <typename T>
        struct cancellable_async_operation
        {
            com_ptr<void> m_ptr;
            using promise_type = cancellable_promise<T>;

            cancellable_async_operation() noexcept = default;
            explicit cancellable_async_operation(com_ptr<void> const& ptr) noexcept : m_ptr(ptr) {}

            void* get_abi() const noexcept { return m_ptr.get(); }
            explicit operator bool() const noexcept { return m_ptr != nullptr; }

            auto operator co_await() const noexcept
            {
                // m_ptr holds the state's COM interface pointer (get_abi());
                // recover the owning state through the iface base
                auto* iface = static_cast<inspectable_iface_base<cancellable_state<T>>*>(m_ptr.get());
                return cancellable_awaiter<T>{ iface ? iface->m_owner : nullptr };
            }
        };

        // IAsyncAction variant (void result)
        export using cancellable_async_action = cancellable_async_operation<void>;

        // the promise: owns a reference to the state for the whole coroutine
        // lifetime and reports completion from final_suspend.
        export template <typename T>
        struct cancellable_promise
        {
            com_ptr<void> m_state;
            cancellable_state<T>* m_raw = nullptr;
            cancellation_token m_token;

            cancellable_promise()
            {
                // the com_ptr holds the state's COM interface pointer, not the
                // raw state: com_ptr refcounts through the COM vtable
                m_raw = new cancellable_state<T>();
                m_state = com_ptr<void>::attach(m_raw->get_abi());
                m_token = cancellation_token{ m_raw->m_cancel };
            }

            std::suspend_never initial_suspend() noexcept { return {}; }

            std::suspend_never final_suspend() noexcept
            {
                m_raw->complete();
                return {};
            }

            cancellable_async_operation<T> get_return_object() noexcept
            {
                return cancellable_async_operation<T>{ m_state };
            }

            void unhandled_exception() noexcept
            {
                try
                {
                    throw;
                }
                catch (hresult_error const& e)
                {
                    m_raw->set_error(e.value());
                }
                catch (std::bad_alloc const&)
                {
                    m_raw->set_error(hr_out_of_memory);
                }
                catch (...)
                {
                    m_raw->set_error(hr_error);
                }
            }

            void return_value(T value)
            {
                m_raw->set_result(std::move(value));
            }
        };

        // void specialization: no return_value, IAsyncAction semantics
        // (explicit specializations of exported templates are auto-exported)
        template <>
        struct cancellable_promise<void>
        {
            com_ptr<void> m_state;
            cancellable_state<void>* m_raw = nullptr;
            cancellation_token m_token;

            cancellable_promise()
            {
                m_raw = new cancellable_state<void>();
                m_state = com_ptr<void>::attach(m_raw->get_abi());
                m_token = cancellation_token{ m_raw->m_cancel };
            }

            std::suspend_never initial_suspend() noexcept { return {}; }

            std::suspend_never final_suspend() noexcept
            {
                m_raw->complete();
                return {};
            }

            cancellable_async_operation<void> get_return_object() noexcept
            {
                return cancellable_async_operation<void>{ m_state };
            }

            void unhandled_exception() noexcept
            {
                try
                {
                    throw;
                }
                catch (hresult_error const& e)
                {
                    m_raw->set_error(e.value());
                }
                catch (std::bad_alloc const&)
                {
                    m_raw->set_error(hr_out_of_memory);
                }
                catch (...)
                {
                    m_raw->set_error(hr_error);
                }
            }

            void return_void()
            {
            }
        };
)cppwinc";
}