#include "ProbeApo.h"
#include "Guids.h"
#include "Log.h"
#include <new>

namespace
{
    std::atomic<long> g_serverLocks{0};

    class ClassFactory final : public IClassFactory
    {
    public:
        explicit ClassFactory(ApoSlot slot) noexcept : m_slot(slot) {}

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
            if (outer)
                return CLASS_E_NOAGGREGATION;

            CProbeApo* apo = new (std::nothrow) CProbeApo(m_slot);
            if (!apo)
                return E_OUTOFMEMORY;
            HRESULT hr = apo->QueryInterface(riid, ppv);
            apo->Release();  // drop the constructor's reference
            return hr;
        }
        STDMETHOD(LockServer)(BOOL lock) override
        {
            lock ? ++g_serverLocks : --g_serverLocks;
            return S_OK;
        }

    private:
        const ApoSlot m_slot;
    };

    ClassFactory g_factoryMfx(ApoSlot::MFX);
    ClassFactory g_factoryEfx(ApoSlot::EFX);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID* ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (rclsid == CLSID_WebrtcVoiceApoMFX)
        return g_factoryMfx.QueryInterface(riid, ppv);
    if (rclsid == CLSID_WebrtcVoiceApoEFX)
        return g_factoryEfx.QueryInterface(riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow()
{
    return (CProbeApo::LiveObjects() == 0 && g_serverLocks == 0) ? S_OK : S_FALSE;
}
