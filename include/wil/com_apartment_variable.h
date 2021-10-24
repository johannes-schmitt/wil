//*********************************************************
//
//    Copyright (c) Microsoft. All rights reserved.
//    This code is licensed under the MIT License.
//    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
//    ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
//    TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//    PARTICULAR PURPOSE AND NONINFRINGEMENT.
//
//*********************************************************
#ifndef __WIL_COM_APARTMENT_VARIABLE_INCLUDED
#define __WIL_COM_APARTMENT_VARIABLE_INCLUDED

#include <unordered_map>
#include <any>
#include <type_traits>
#include "com.h"
#include "cppwinrt.h"
#include <roapi.h>
#include <objidl.h>
#include "result_macros.h"
#include <winrt/Windows.Foundation.h>

#ifndef WIL_ENABLE_EXCEPTIONS
#error This header requires exceptions
#endif

namespace wil
{
    // Determine if apartment variables are supported in the current process context.
    // Prior to build 22365, the APIs needed to create apartment variables (e.g. RoGetApartmentIdentifier)
    // failed for unpackaged processes. For MS people, see http://task.ms/31861017 for details.
    // APIs needed to implement apartment variables did not work in non-packaged processes.
    inline bool are_apartment_variables_supported()
    {
        unsigned long long apartmentId{};
        return RoGetApartmentIdentifier(&apartmentId) != HRESULT_FROM_WIN32(ERROR_API_UNAVAILABLE);
    }

    // TODO: COM will implicitly rundown the apartment registration when it invokes a handler
    // and it blocks calling unregister when executing the callback. So be careful to release()
    // this if the callback is invoked.
    // Perhaps this should not be an RAII type given how strange the behavior is.
    using unique_apartment_shutdown_registration = unique_any<APARTMENT_SHUTDOWN_REGISTRATION_COOKIE, decltype(&::RoUnregisterForApartmentShutdown), ::RoUnregisterForApartmentShutdown>;

    struct apartment_variable_platform
    {
        static unsigned long long GetApartmentId()
        {
            unsigned long long apartmentId{};
            FAIL_FAST_IF_FAILED(RoGetApartmentIdentifier(&apartmentId));
            return apartmentId;
        }

        static auto RegisterForApartmentShutdown(IApartmentShutdown* observer)
        {
            unsigned long long id{};
            typename shutdown_type cookie;
            THROW_IF_FAILED(RoRegisterForApartmentShutdown(observer, &id, cookie.put()));
            return cookie;
        }

        static void UnRegisterForApartmentShutdown(APARTMENT_SHUTDOWN_REGISTRATION_COOKIE cookie)
        {
            FAIL_FAST_IF_FAILED(RoUnregisterForApartmentShutdown(cookie));
        }

        static auto CoInitializeEx(DWORD coinitFlags = 0 /*COINIT_MULTITHREADED*/)
        {
            return wil::CoInitializeEx(coinitFlags);
        }

        // disable the test hook
        inline static constexpr unsigned long AsyncRundownDelayForTestingRaces = INFINITE;

        using shutdown_type = wil::unique_apartment_shutdown_registration;
    };

    namespace details
    {
        struct any_maker_base
        {
            std::any(*adapter)(void*);
            void* inner;

            std::any operator()()
            {
                return adapter(inner);
            }
        };

        template<typename T>
        struct any_maker : any_maker_base
        {
            any_maker()
            {
                adapter = [](auto) -> std::any { return T{}; };
            }

            any_maker(T(*maker)())
            {
                adapter = [](auto maker) -> std::any { return reinterpret_cast<T(*)()>(maker)(); };
                inner = reinterpret_cast<void*>(maker);
            }

            template<typename F>
            any_maker(F&& f)
            {
                adapter = [](auto maker) -> std::any { return reinterpret_cast<F*>(maker)[0](); };
                inner = std::addressof(f);
            }
        };

        template<typename test_hook = apartment_variable_platform>
        struct apartment_variable_base
        {
            inline static winrt::slim_mutex s_lock;

            struct apartment_variable_storage
            {
                apartment_variable_storage(apartment_variable_storage&& other)
                {
                    cookie = std::move(other.cookie);
                    context = std::move(other.context);
                    variables = std::move(other.variables);
                }

                apartment_variable_storage(typename test_hook::shutdown_type&& cookie_) : cookie(std::move(cookie_))
                {
                }

                winrt::apartment_context context;
                typename test_hook::shutdown_type cookie;
                std::unordered_map<apartment_variable_base<test_hook>*, std::any> variables;
            };

            // Apartment id -> variable storage.
            // Variables are stored using the address of the global variable as the key.
            inline static std::unordered_map<unsigned long long, apartment_variable_storage> s_apartmentStorage;

            apartment_variable_base() = default;
            ~apartment_variable_base()
            {
                if (!ProcessShutdownInProgress())
                {
                    clear_all_apartments_async();
                }
            }

            // non-copyable, non-assignable
            apartment_variable_base(apartment_variable_base const&) = delete;
            void operator=(apartment_variable_base const&) = delete;

            // get current value or throw if no value has been set
            std::any& get_existing()
            {
                if (auto any = get_if())
                {
                    return *any;
                }
                THROW_HR(E_NOT_SET);
            }

            static apartment_variable_storage* get_current_apartment_variable_storage()
            {
                auto storage = s_apartmentStorage.find(test_hook::GetApartmentId());
                if (storage != s_apartmentStorage.end())
                {
                    return &storage->second;
                }
                return nullptr;
            }

            apartment_variable_storage* ensure_current_apartment_variables()
            {
                auto variables = get_current_apartment_variable_storage();
                if (variables)
                {
                    return variables;
                }

                struct ApartmentObserver : public winrt::implements<ApartmentObserver, IApartmentShutdown>
                {
                    void STDMETHODCALLTYPE OnUninitialize(unsigned long long apartmentId) noexcept override
                    {
                        // This code runs at apartment rundown so be careful to avoid deadlocks by
                        // extracting the variables under the lock then release them outside.
                        auto variables = [apartmentId]()
                        {
                            auto lock = winrt::slim_lock_guard(s_lock);
                            return s_apartmentStorage.extract(apartmentId);
                        }();
                        WI_ASSERT(variables.key() == apartmentId);
                        // The system implicitly releases the shutdown observer
                        // after invoking the callback and does not allow calling unregister
                        // in the callback. So release the reference to the registration.
                        variables.mapped().cookie.release();
                    }
                };
                auto shutdownRegistration = test_hook::RegisterForApartmentShutdown(winrt::make<ApartmentObserver>().get());
                return &s_apartmentStorage.insert({ test_hook::GetApartmentId(), apartment_variable_storage(std::move(shutdownRegistration)) }).first->second;
            }

            // get current value or custom-construct one on demand
            template<typename T>
            std::any& get_or_create(any_maker<T>&& creator)
            {
                apartment_variable_storage* variable_storage{};

                { // scope for lock
                    auto lock = winrt::slim_lock_guard(s_lock);
                    variable_storage = ensure_current_apartment_variables();

                    auto variable = variable_storage->variables.find(this);
                    if (variable != variable_storage->variables.end())
                    {
                        return variable->second;
                    }
                } // drop the lock

                // create the object outside the lock to avoid reentrant deadlock
                auto value = creator();

                auto insert_lock = winrt::slim_lock_guard(s_lock);
                // The insertion may fail if creator() recursively caused itself to be created,
                // in which case we return the existing object and the falsely-created one is discarded.
                return variable_storage->variables.insert({ this, std::move(value) }).first->second;
            }

            // get pointer to current value or nullptr if no value has been set
            std::any* get_if()
            {
                auto lock = winrt::slim_lock_guard(s_lock);

                if (auto variable_storage = get_current_apartment_variable_storage())
                {
                    auto variable = variable_storage->variables.find(this);
                    if (variable != variable_storage->variables.end())
                    {
                        return &(variable->second);
                    }
                }
                return nullptr;
            }

            // replace or create the current value, fail fasts if the value is not already stored
            void set(std::any value)
            {
                // release value, with the swapped value, outside of the lock
                {
                    auto lock = winrt::slim_lock_guard(s_lock);
                    auto storage = s_apartmentStorage.find(test_hook::GetApartmentId());
                    FAIL_FAST_IF(storage == s_apartmentStorage.end());
                    auto& variable_storage = storage->second;
                    auto variable = variable_storage.variables.find(this);
                    FAIL_FAST_IF(variable == variable_storage.variables.end());
                    variable->second.swap(value);
                }
            }

            // remove any current value
            void clear()
            {
                auto lock = winrt::slim_lock_guard(s_lock);
                if (auto variable_storage = get_current_apartment_variable_storage())
                {
                    variable_storage->variables.erase(this);
                    if (variable_storage->variables.size() == 0)
                    {
                        s_apartmentStorage.erase(test_hook::GetApartmentId());
                    }
                }
            }

            winrt::Windows::Foundation::IAsyncAction clear_all_apartments_async()
            {
                // gather all the apartments that hold objects we need to destruct
                // (do not gather the objects themselves, because the apartment might
                // destruct before we get around to it, and we should let the apartment
                // destruct the object while it still can).

                std::vector<winrt::apartment_context> contexts;
                { // scope for lock
                    auto lock = winrt::slim_lock_guard(s_lock);
                    for (auto& [id, storage] : s_apartmentStorage)
                    {
                        auto variable = storage.variables.find(this);
                        if (variable != storage.variables.end())
                        {
                            contexts.push_back(storage.context);
                        }
                    }
                }

                if (contexts.empty())
                {
                    co_return;
                }

                wil::unique_mta_usage_cookie mta_reference; // need to extend the MTA due to async cleanup
                FAIL_FAST_IF_FAILED(CoIncrementMTAUsage(mta_reference.put()));

                // From a background thread hop into each apartment to run down the object
                // if it's still there.
                co_await winrt::resume_background();

                // This hook enables testing the case where execution of this method loses the race with
                // apartment rundown by other means.
                if constexpr (test_hook::AsyncRundownDelayForTestingRaces != INFINITE)
                {
                    Sleep(test_hook::AsyncRundownDelayForTestingRaces);
                }

                for (auto&& context : contexts)
                {
                    try
                    {
                        WI_ASSERT(!ProcessShutdownInProgress());
                        co_await context;
                        WI_ASSERT(!ProcessShutdownInProgress());
                        clear();
                    }
                    catch (winrt::hresult_error const& e)
                    {
                        // Ignore failure if apartment ran down before we could clean it up.
                        // The object already ran down as part of apartment cleanup.
                        if ((e.code() != RPC_E_SERVER_DIED_DNE) &&
                            (e.code() != RPC_E_DISCONNECTED))
                        {
                            throw;
                        }
                    }
                    catch (...)
                    {
                        FAIL_FAST();
                    }
                }
            }

            static const auto& storage()
            {
                return s_apartmentStorage;
            }

            static size_t current_apartment_variable_count()
            {
                auto lock = winrt::slim_lock_guard(s_lock);
                if (auto variable_storage = get_current_apartment_variable_storage())
                {
                    return variable_storage->variables.size();
                }
                return 0;
            }
        };
    }

    // Apartment variables enable storing COM objects safely in globals
    // or in components that use apartment affine objects that need to
    // be created and used only in the same apartment.
    //
    // For global objects (namespace scope variables or function/class statics)
    // 1) Implemented in a dll, inform wil about the dll unload state by forwarding
    //    DLL entry point calls wil::DLLMain().
    // 2) For exes call wil::DLLMain(..., DLL_PROCESS_DETACH, reinterpret_cast<void*>(1)).
    //
    // OR
    //
    // 3) Use wil::object_without_destructor_on_shutdown<wil::apartment_variable<T>>.
    //
    // These are necessary to avoid executing the async rundown inappropriately at
    // module unload or process rundown.

    template<typename T, typename test_hook = wil::apartment_variable_platform>
    struct apartment_variable : details::apartment_variable_base<test_hook>
    {
        using base = details::apartment_variable_base<test_hook>;

        // Get current value or throw if no value has been set.
        T& get_existing() { return std::any_cast<T&>(base::get_existing()); }

        // Get current value or default-construct one on demand.
        T& get_or_create()
        {
            return std::any_cast<T&>(base::get_or_create(details::any_maker<T>()));
        }

        // Get current value or custom-construct one on demand.
        template<typename F>
        T& get_or_create(F&& f)
        {
            return std::any_cast<T&>(base::get_or_create(details::any_maker<T>(std::forward<F>(f))));
        }

        // get pointer to current value or nullptr if no value has been set
        T* get_if() { return std::any_cast<T>(base::get_if()); }

        // replace or create the current value, fail fasts if the value is not already stored
        template<typename V> void set(V&& value) { return base::set(std::forward<V>(value)); }

        // Clear the value in the current apartment.
        using base::clear;

        // Asynchronously clear the value in all apartments it is present in.
        using base::clear_all_apartments_async;

        // For testing only.
        // 1) To observe the state of the storage in the debugger assign this to
        // a temporary variable (const&) and watch its contents.
        // 2) Use this to test the implementation.
        using base::storage;
        // For testing only. The number of variables in the current apartment.
        using base::current_apartment_variable_count;
    };
}

#endif // __WIL_COM_APARTMENT_VARIABLE_INCLUDED
