#include "search_candidate_provider.h"

#include "ime_config.h"
#include "ime_trace.h"
#include "text_service.h"

#include <algorithm>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace
{
class search_candidate_string final : public ITfCandidateString
{
public:
    search_candidate_string(std::wstring value, ULONG index)
        : ref_(1), value_(std::move(value)), index_(index)
    {
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override
    {
        if (!ppvObj)
            return E_INVALIDARG;
        *ppvObj = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfCandidateString))
        {
            *ppvObj = static_cast<ITfCandidateString*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG ref = InterlockedDecrement(&ref_);
        if (ref == 0)
            delete this;
        return static_cast<ULONG>(ref);
    }

    STDMETHODIMP GetString(BSTR* pbstr) override
    {
        if (!pbstr)
            return E_INVALIDARG;
        *pbstr = SysAllocStringLen(value_.data(), static_cast<UINT>(value_.size()));
        return *pbstr ? S_OK : E_OUTOFMEMORY;
    }

    STDMETHODIMP GetIndex(ULONG* pnIndex) override
    {
        if (!pnIndex)
            return E_INVALIDARG;
        *pnIndex = index_;
        return S_OK;
    }

private:
    LONG ref_;
    std::wstring value_;
    ULONG index_;
};

HRESULT make_candidate_string(const std::vector<std::wstring>& candidates,
                              ULONG index,
                              ITfCandidateString** result)
{
    if (!result)
        return E_INVALIDARG;
    *result = nullptr;
    if (index >= candidates.size())
        return E_INVALIDARG;

    try
    {
        auto* value = new (std::nothrow) search_candidate_string(candidates[index], index);
        if (!value)
            return E_OUTOFMEMORY;
        *result = value;
        return S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return E_OUTOFMEMORY;
    }
}

class search_candidate_enumerator final : public IEnumTfCandidates
{
public:
    search_candidate_enumerator(std::vector<std::wstring> candidates, ULONG index)
        : ref_(1), candidates_(std::move(candidates)), index_(index)
    {
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override
    {
        if (!ppvObj)
            return E_INVALIDARG;
        *ppvObj = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumTfCandidates))
        {
            *ppvObj = static_cast<IEnumTfCandidates*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG ref = InterlockedDecrement(&ref_);
        if (ref == 0)
            delete this;
        return static_cast<ULONG>(ref);
    }

    STDMETHODIMP Clone(IEnumTfCandidates** ppEnum) override
    {
        if (!ppEnum)
            return E_INVALIDARG;
        *ppEnum = nullptr;
        try
        {
            auto* clone = new (std::nothrow) search_candidate_enumerator(candidates_, index_);
            if (!clone)
                return E_OUTOFMEMORY;
            *ppEnum = clone;
            return S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
    }

    STDMETHODIMP Next(ULONG count, ITfCandidateString** values, ULONG* fetched) override
    {
        if (!values || (!fetched && count != 1))
            return E_INVALIDARG;
        if (fetched)
            *fetched = 0;

        ULONG produced = 0;
        while (produced < count && index_ < candidates_.size())
        {
            const HRESULT hr = make_candidate_string(candidates_, index_, &values[produced]);
            if (FAILED(hr))
            {
                for (ULONG i = 0; i < produced; ++i)
                    values[i]->Release();
                return hr;
            }
            ++produced;
            ++index_;
        }

        if (fetched)
            *fetched = produced;
        return produced == count ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override
    {
        index_ = 0;
        return S_OK;
    }

    STDMETHODIMP Skip(ULONG count) override
    {
        const size_t remaining = candidates_.size() - std::min<size_t>(index_, candidates_.size());
        const size_t skipped = std::min<size_t>(count, remaining);
        index_ += static_cast<ULONG>(skipped);
        return skipped == count ? S_OK : S_FALSE;
    }

private:
    LONG ref_;
    std::vector<std::wstring> candidates_;
    ULONG index_;
};

class search_candidate_list final : public ITfCandidateList
{
public:
    explicit search_candidate_list(std::vector<std::wstring> candidates)
        : ref_(1), candidates_(std::move(candidates))
    {
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override
    {
        if (!ppvObj)
            return E_INVALIDARG;
        *ppvObj = nullptr;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfCandidateList))
        {
            *ppvObj = static_cast<ITfCandidateList*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&ref_));
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG ref = InterlockedDecrement(&ref_);
        if (ref == 0)
            delete this;
        return static_cast<ULONG>(ref);
    }

    STDMETHODIMP EnumCandidates(IEnumTfCandidates** ppEnum) override
    {
        if (!ppEnum)
            return E_INVALIDARG;
        *ppEnum = nullptr;
        try
        {
            auto* enumerator = new (std::nothrow) search_candidate_enumerator(candidates_, 0);
            if (!enumerator)
                return E_OUTOFMEMORY;
            *ppEnum = enumerator;
            return S_OK;
        }
        catch (const std::bad_alloc&)
        {
            return E_OUTOFMEMORY;
        }
    }

    STDMETHODIMP GetCandidate(ULONG index, ITfCandidateString** candidate) override
    {
        return make_candidate_string(candidates_, index, candidate);
    }

    STDMETHODIMP GetCandidateNum(ULONG* count) override
    {
        if (!count)
            return E_INVALIDARG;
        *count = static_cast<ULONG>(candidates_.size());
        return S_OK;
    }

    STDMETHODIMP SetResult(ULONG index, TfCandidateResult result) override
    {
        UNREFERENCED_PARAMETER(result);
        return index < candidates_.size() ? S_OK : E_INVALIDARG;
    }

private:
    LONG ref_;
    std::vector<std::wstring> candidates_;
};
}

search_candidate_provider::search_candidate_provider(text_service* service)
    : ref_(1), service_(service)
{
    if (service_)
        service_->AddRef();
}

search_candidate_provider::~search_candidate_provider()
{
    if (service_)
        service_->Release();
}

STDMETHODIMP search_candidate_provider::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj)
        return E_INVALIDARG;
    *ppvObj = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfFunction) ||
        IsEqualIID(riid, IID_ITfFnSearchCandidateProvider))
    {
        *ppvObj = static_cast<ITfFnSearchCandidateProvider*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) search_candidate_provider::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&ref_));
}

STDMETHODIMP_(ULONG) search_candidate_provider::Release()
{
    const LONG ref = InterlockedDecrement(&ref_);
    if (ref == 0)
        delete this;
    return static_cast<ULONG>(ref);
}

STDMETHODIMP search_candidate_provider::GetDisplayName(BSTR* pbstrName)
{
    if (!pbstrName)
        return E_INVALIDARG;
    *pbstrName = SysAllocString(IME_DESCRIPTION);
    return *pbstrName ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP search_candidate_provider::GetSearchCandidates(BSTR bstrQuery,
                                                             BSTR bstrApplicationId,
                                                             ITfCandidateList** pplist)
{
    UNREFERENCED_PARAMETER(bstrApplicationId);
    if (!pplist)
        return E_INVALIDARG;
    *pplist = nullptr;
    if (!service_)
        return E_UNEXPECTED;

    std::vector<std::wstring> candidates;
    const HRESULT hr = service_->BuildSearchCandidates(bstrQuery, candidates);
    if (FAILED(hr))
        return hr;

    auto* list = new (std::nothrow) search_candidate_list(std::move(candidates));
    if (!list)
        return E_OUTOFMEMORY;
    *pplist = list;
    ime_tracef(L"SearchCandidates", L"query_len=%u", bstrQuery ? SysStringLen(bstrQuery) : 0);
    return S_OK;
}

STDMETHODIMP search_candidate_provider::SetResult(BSTR bstrQuery,
                                                   BSTR bstrApplicationID,
                                                   BSTR bstrResult)
{
    UNREFERENCED_PARAMETER(bstrQuery);
    UNREFERENCED_PARAMETER(bstrApplicationID);
    UNREFERENCED_PARAMETER(bstrResult);
    return E_NOTIMPL;
}
