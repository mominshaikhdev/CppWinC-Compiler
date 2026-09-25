// component_data.ixx - module component_data.
// The projection-side authoring helpers emitted into the Windows.Foundation
// module: the thin public factories that wrap the winrt_base collection and
// coroutine machinery (single_threaded_vector/map, attach_cancellable,
// when_all/when_any) with the projected types, which winrt_base itself
// cannot name.
// Copyright (c) 2026 Momin Shaikh. All rights reserved.

module;

#include <string_view>

export module component_data;

namespace cppwinc::strings
{
    // Emitted into winrt.Windows.Foundation, where the projected collection
    // and async types are nameable.
    export inline constexpr std::string_view component_authoring_ixx = R"cppwinc(
// component authoring helpers: single-threaded collections, cancellable
// coroutines, and async combinators — thin wrappers over winrt_base
// machinery (the implementations live in winrt_base, in winrt::impl)
namespace winrt
{
    // single-threaded collection factories
    export template <typename T>
    Windows::Foundation::Collections::IObservableVector<T> single_threaded_vector()
    {
        void* abi = impl::make_single_threaded_vector<T>();
        return Windows::Foundation::Collections::IObservableVector<T>::from_abi(abi);
    }

    export template <typename T>
    Windows::Foundation::Collections::IObservableVector<T> single_threaded_observable_vector()
    {
        return single_threaded_vector<T>();
    }

    export template <typename K, typename V>
    Windows::Foundation::Collections::IObservableMap<K, V> single_threaded_map()
    {
        void* abi = impl::make_single_threaded_map<K, V>();
        return Windows::Foundation::Collections::IObservableMap<K, V>::from_abi(abi);
    }

    export template <typename K, typename V>
    Windows::Foundation::Collections::IObservableMap<K, V> single_threaded_observable_map()
    {
        return single_threaded_map<K, V>();
    }

    // read a boxed value out of an IMap (PropertySet-style) by key; the
    // lookup and the unbox both go through the strict typed paths, so a
    // missing key or a wrong boxed type throws (hresult_not_found from
    // Lookup, hresult_invalid_argument from unbox_value)
    export template <typename T>
    T unbox_value_at(Windows::Foundation::Collections::IMap<winrt::hstring, winrt::IInspectable> const& map, winrt::hstring const& key)
    {
        return unbox_value<T>(map.Lookup(key));
    }

    // wrap a cancellable coroutine's return object into the projected async
    // interface (takes a reference, so the returned wrapper owns the operation)
    export template <typename T>
    Windows::Foundation::IAsyncOperation<T> attach_cancellable(impl::cancellable_async_operation<T> const& operation)
    {
        void* abi = operation.get_abi();
        if (abi)
        {
            IUnknown::addref_raw(abi);
        }
        return Windows::Foundation::IAsyncOperation<T>::from_abi(abi);
    }

    export inline Windows::Foundation::IAsyncAction attach_cancellable(impl::cancellable_async_action const& operation)
    {
        void* abi = operation.get_abi();
        if (abi)
        {
            IUnknown::addref_raw(abi);
        }
        return Windows::Foundation::IAsyncAction::from_abi(abi);
    }

    // async combinators over a set of actions; each completes the returned
    // IAsyncAction when all (when_all) or any (when_any) inputs complete
    export inline Windows::Foundation::IAsyncAction when_all(std::vector<Windows::Foundation::IAsyncAction> const& actions)
    {
        std::vector<void*> abis;
        abis.reserve(actions.size());
        for (auto const& action : actions)
        {
            abis.push_back(action.get_abi());
        }
        return Windows::Foundation::IAsyncAction::from_abi(impl::make_when_all(abis.data(), abis.size()));
    }

    export inline Windows::Foundation::IAsyncAction when_all(std::initializer_list<Windows::Foundation::IAsyncAction> actions)
    {
        return when_all(std::vector<Windows::Foundation::IAsyncAction>(actions));
    }

    export inline Windows::Foundation::IAsyncAction when_any(std::vector<Windows::Foundation::IAsyncAction> const& actions)
    {
        std::vector<void*> abis;
        abis.reserve(actions.size());
        for (auto const& action : actions)
        {
            abis.push_back(action.get_abi());
        }
        return Windows::Foundation::IAsyncAction::from_abi(impl::make_when_any(abis.data(), abis.size()));
    }

    export inline Windows::Foundation::IAsyncAction when_any(std::initializer_list<Windows::Foundation::IAsyncAction> actions)
    {
        return when_any(std::vector<Windows::Foundation::IAsyncAction>(actions));
    }
}
)cppwinc";
}
