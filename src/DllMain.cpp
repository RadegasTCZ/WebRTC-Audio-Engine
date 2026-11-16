// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
#include "EngineApo.h"
#include "Guids.h"
#include "Log.h"
#include "Settings.h"
#include <new>

namespace
{
    std::atomic<long> g_serverLocks{0};

    class ClassFactory final : public IClassFactory
    {
    public:

        STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
        {
            if (!ppv)
                return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory))
            {
                *ppv = static_cast<IClassFactory*>(this);
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        // Static factory objects: no real ref counting needed.
        STDMETHOD_(ULONG, AddRef)() override { return 2; }
        STDMETHOD_(ULONG, Release)() override { return 1; }

        STDMETHOD(CreateInstance)(IUnknown* outer, REFIID riid, void** ppv) override
        {
            if (!ppv)
                return E_POINTER;
            *ppv = nullptr;
            // The audio engine aggregates APOs. COM rule: an aggregated object may only be
            // asked for IUnknown at creation time.
            if (outer && riid != __uuidof(IUnknown))
                return CLASS_E_NOAGGREGATION;

            CEngineApo* apo = new (std::nothrow) CEngineApo(outer);
            if (!apo)
                return E_OUTOFMEMORY;
            IUnknown* inner = apo->NonDelegatingUnknown();
            HRESULT hr = inner->QueryInterface(riid, ppv);
            inner->Release();  // drop the constructor's reference
            return hr;
        }
        STDMETHOD(LockServer)(BOOL lock) override
        {
            lock ? ++g_serverLocks : --g_serverLocks;
            return S_OK;
        }
    };

    ClassFactory g_factoryPack;
}

extern void (*g_afterSettingsChanged)() noexcept;

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        g_afterSettingsChanged = &SignalSystemEffectsChanged;  // keep Windows Settings in sync with our settings
    }
    return TRUE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID* ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (rclsid == CLSID_WebrtcAudioEnginePack)
        return g_factoryPack.QueryInterface(riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow()
{
    // The settings watcher (a thread-pool wait) lives for the whole process: never unload once started.
    return (CEngineApo::LiveObjects() == 0 && g_serverLocks == 0 && !settings::WatcherStarted()) ? S_OK : S_FALSE;
}
