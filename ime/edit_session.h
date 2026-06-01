#pragma once

#include "globals.h"
#include <msctf.h>
#include <string>

// 编辑会话类 - 用于在TSF中安全地插入文本
class edit_session : public ITfEditSession
{
public:
	edit_session(ITfContext* context, const std::wstring& text);
	virtual ~edit_session();

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override;
	STDMETHODIMP_(ULONG) AddRef() override;
	STDMETHODIMP_(ULONG) Release() override;

	// ITfEditSession
	STDMETHODIMP DoEditSession(TfEditCookie ec) override;

private:
	LONG ref_;
	ITfContext* context_;
	std::wstring text_;
};
