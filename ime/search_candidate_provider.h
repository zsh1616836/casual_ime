#pragma once

#include <ctffunc.h>

class text_service;

class search_candidate_provider final : public ITfFnSearchCandidateProvider
{
public:
    explicit search_candidate_provider(text_service* service);
    ~search_candidate_provider();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    STDMETHODIMP GetDisplayName(BSTR* pbstrName) override;
    STDMETHODIMP GetSearchCandidates(BSTR bstrQuery,
                                     BSTR bstrApplicationId,
                                     ITfCandidateList** pplist) override;
    STDMETHODIMP SetResult(BSTR bstrQuery,
                           BSTR bstrApplicationID,
                           BSTR bstrResult) override;

private:
    LONG ref_;
    text_service* service_;
};
