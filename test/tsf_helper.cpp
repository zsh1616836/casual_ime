#include <iostream>
#include "tsf_helper.h"
#include "Input.h"
#include <comdef.h>
#include <tchar.h>

char m_candidates[1024];

InputManager::InputManager()
{
	m_pThreadMgr = nullptr;
	m_pic = nullptr;
	m_ObjRefCount = 0;
	m_tf_client_id = 0;
	dwUIElementSinkCookie = 0;
	dwThreadMgrEventCookie = 0;
	dwTextEditSink = 0;
}

InputManager::~InputManager()
{
	Dispose();
}

bool InputManager::Initialize()
{
	const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(hr)) return false;
	do
	{
		if (FAILED(
			CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER, IID_ITfThreadMgrEx, (void**)&m_pThreadMgr
			)))
		{
			break;
		}

		//使用Ex类的激活TSF方式，可以关闭搜狗输入法的OS候选词框
		if (FAILED(m_pThreadMgr->ActivateEx(&m_tf_client_id, TF_TMAE_UIELEMENTENABLEDONLY)))
		{
			break;
		}

		ITfSource* source;
		if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<LPVOID*>(&source))))
		{
			source->AdviseSink(IID_ITfUIElementSink, static_cast<ITfUIElementSink*>(this), &dwUIElementSinkCookie);
			source->AdviseSink(IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this),
			                   &dwThreadMgrEventCookie);
			source->Release();
		}
		return true;
	}
	while (false);

	if (m_pThreadMgr)
	{
		m_pThreadMgr->Release();
		m_pThreadMgr = nullptr;
	}
	return false;
}

bool InputManager::Dispose()
{
	ITfSource* source;
	if (m_pic)
	{
		if (SUCCEEDED(m_pic->QueryInterface(IID_ITfSource, reinterpret_cast<LPVOID*>(&source))))
		{
			source->UnadviseSink(dwTextEditSink);
			dwTextEditSink = 0;
			source->Release();
		}
	}


	if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfSource, reinterpret_cast<LPVOID*>(&source))))
	{
		source->UnadviseSink(dwThreadMgrEventCookie);
		source->UnadviseSink(dwUIElementSinkCookie);
		source->Release();
		dwThreadMgrEventCookie = 0;
		dwUIElementSinkCookie = 0;
	}

	m_pThreadMgr->Deactivate();
	m_pThreadMgr->Release();
	CoUninitialize();
	return true;
}

HRESULT InputManager::OnInitDocumentMgr(ITfDocumentMgr* pdim)
{
	return S_OK;
}

HRESULT InputManager::OnUninitDocumentMgr(ITfDocumentMgr* pdim)
{
	return S_OK;
}

HRESULT InputManager::OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus)
{
	ITfSource* source = nullptr;
	if (dwTextEditSink != 0)
	{
		if (SUCCEEDED(m_pic->QueryInterface(IID_ITfSource, reinterpret_cast<LPVOID*>(&source))))
		{
			source->UnadviseSink(dwTextEditSink);
			dwTextEditSink = 0;
			source->Release();
		}
	}
	if (pdimFocus == nullptr)
		return S_OK;
	do
	{
		HRESULT hr = pdimFocus->GetTop(&m_pic);
		if (FAILED(hr)) break;
		hr = m_pic->QueryInterface(IID_ITfSource, reinterpret_cast<LPVOID*>(&source));
		if (FAILED(hr)) break;
		hr = source->AdviseSink(IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this), &dwTextEditSink);
		source->Release();
		if (SUCCEEDED(hr))
		{
			return S_OK;
		}
	}
	while (false);

	if (m_pic)
		m_pic->Release();
	if (source)
		source->Release();
	return S_FALSE;
}

HRESULT InputManager::OnPushContext(ITfContext* pic)
{
	return S_OK;
}

HRESULT InputManager::OnPopContext(ITfContext* pic)
{
	return S_OK;
}

HRESULT InputManager::OnEndEdit(ITfContext* pic, TfEditCookie ecReadOnly, ITfEditRecord* pEditRecord)
{
	ITfDocumentMgr* pDocMgr = nullptr;
	if (SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgr)))
	{
		ITfContext* pContext = nullptr;
		if (pDocMgr != nullptr && SUCCEEDED(pDocMgr->GetTop(&pContext)))
		{
			ITfContextComposition* pComposition = nullptr;
			if (SUCCEEDED(pContext->QueryInterface(IID_ITfContextComposition, reinterpret_cast<void**>(&pComposition))))
			{
				IEnumITfCompositionView* pEnumComposition = nullptr;
				if (SUCCEEDED(pComposition->EnumCompositions(&pEnumComposition)))
				{
					ITfCompositionView* pCompositionView = nullptr;
					HRESULT hr = pEnumComposition->Next(1, &pCompositionView, NULL);
					if (hr != S_OK) OnCompositionReceived((char*)"", 0);
					while (hr == S_OK)
					{
						ITfRange* pvRange;
						ULONG writeSize;
						WCHAR wStr[WCHAR_MAX];
						if (pCompositionView)
						{
							pCompositionView->GetRange(&pvRange);
							pvRange->GetText(true, TF_TF_IGNOREEND, wStr, WCHAR_MAX, &writeSize);
							wStr[writeSize] = NULL;

							BSTR bstr = SysAllocString(wStr);
							_bstr_t b = bstr;
							const wchar_t* wstr = b;
							char temp[1024];
							const int ret = WcharToChar(wstr, static_cast<int>(wcslen(wstr)), temp, 1024);
							if (ret > 0) OnCompositionReceived(temp, ret);
							SysFreeString(bstr);
							pvRange->Release();
							pCompositionView->Release();
						}
						hr = pEnumComposition->Next(1, &pCompositionView, NULL);
					}
					pEnumComposition->Release();
				}
				pComposition->Release();
			}
			pContext->Release();
		}
		pDocMgr->Release();
	}
	return S_FALSE;
}

HRESULT InputManager::QueryInterface(const IID& riid, void** ppvObject)
{
	std::cout << "Enter the QueryInterface!" << std::endl;
	*ppvObject = nullptr;
	if (IsEqualIID(riid, IID_ITfTextEditSink))
	{
		*ppvObject = static_cast<ITfTextEditSink*>(this);
	}
	else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
	{
		*ppvObject = static_cast<ITfThreadMgrEventSink*>(this);
	}
	else if (IsEqualIID(riid, IID_ITfUIElementSink))
	{
		*ppvObject = static_cast<ITfUIElementSink*>(this);
	}

	if (*ppvObject)
	{
		(*(LPUNKNOWN*)ppvObject)->AddRef();
		return S_OK;
	}

	return E_NOINTERFACE;
}

ULONG InputManager::AddRef()
{
	return ++m_ObjRefCount;
}

ULONG InputManager::Release()
{
	if (--m_ObjRefCount == 0)
	{
		//delete this;
		return 0; 
	}

	return m_ObjRefCount;
}

HRESULT InputManager::BeginUIElement(DWORD dwUIElementId, BOOL* pbShow)
{
	std::cout << "Enter the BeginUIElement" << std::endl;
	*pbShow = get_os_candidate_form_visible();
	return S_OK;
}

HRESULT InputManager::UpdateUIElement(DWORD dwUIElementId)
{
	ITfUIElementMgr* lpMgr = nullptr;
	ITfCandidateListUIElement* lpCandUI = nullptr;
	ITfUIElement* pElement = nullptr;
	std::cout << "Enter the updateUIElement func!" << std::endl;
	/* 获取候选词，并将其序列化后，传入回调函数*/
	if (SUCCEEDED(m_pThreadMgr->QueryInterface(IID_ITfUIElementMgr, reinterpret_cast<void**>(&lpMgr))))
	{
		if (SUCCEEDED(lpMgr->GetUIElement(dwUIElementId, &pElement)))
		{
			if (SUCCEEDED(
				pElement->QueryInterface(IID_ITfCandidateListUIElement, reinterpret_cast<void**>(&lpCandUI))))
			{
				do
				{
					UINT count; //候选词长度
					UINT current_page;
					UINT page_index[1024];
					UINT selection;
					DWORD updated_flags;
					UINT page_num;

					HRESULT hr = lpCandUI->GetCount(&count);
					if (FAILED(hr)) break;
					hr = lpCandUI->GetCurrentPage(&current_page);
					if (FAILED(hr)) break;
					hr = lpCandUI->GetPageIndex(NULL, 0, &page_num);
					if (FAILED(hr)) break;
					hr = lpCandUI->GetPageIndex(page_index, 1024, &page_num);
					if (FAILED(hr)) break;
					hr = lpCandUI->GetSelection(&selection);
					if (FAILED(hr)) break;
					hr = lpCandUI->GetUpdatedFlags(&updated_flags);
					if (FAILED(hr)) break;

#ifdef _DEBUG1
					std::cout << "\tcount:     " << count << std::endl;
					std::cout << "\tselection: " << selection << std::endl;
					std::cout << "\tcur_page:  " << current_page << std::endl;
					std::cout << "\tpage_index:" << "{ ";
					for (UINT i = 0; i < page_num && i < 5; ++i)
					{
						std::cout << page_index[i] << ", ";
					}
					std::cout << " }" << std::endl;
					std::cout << "\tpage_num:  " << page_num << std::endl;
					std::cout << "\tup_flags:  " << updated_flags << std::endl;
#endif // DEBUG

					BSTR _sss = nullptr;
					int pos = 0;
					if (count > 0)
					{
						UINT start = page_index[current_page];
						UINT end;
						if (current_page == page_num - 1)
							end = count;
						else
							end = page_index[current_page + 1];
						for (UINT i = start; i < end; i++)
						{
							hr = lpCandUI->GetString(i, &_sss);
							if (FAILED(hr))
							{
								OnCandidateReceived(-1, m_candidates, 0); //置空候选词
								break;
							}
							_bstr_t b = _sss;
							/*2021-12-29 zsh 由于不同语言操作系统CodePage的原因，这里不再转换成系统默认编码，而是直接使用Unicode编码来回调候选词列表*/
							wchar_t* wstr = b;
							int ret = b.length() * 2;
							if (pos + 2 + ret > CANDIDATE_MAX_LENGTH) break;
							m_candidates[pos] = static_cast<char>(ret >> 8);
							m_candidates[pos + 1] = static_cast<char>(ret & 255);
							memcpy_s(m_candidates + pos + 2, CANDIDATE_MAX_LENGTH - pos - 2, wstr, ret);
							pos += 2 + ret;
						}
						SysFreeString(_sss);
					}
					if (pos > 0)
					{
						int select = static_cast<int>(selection) - page_index[current_page];
						OnCandidateReceived(select, m_candidates, pos);
					}
				}
				while (false);
				lpCandUI->Release();
			}

			pElement->Release();
		}
		lpMgr->Release();
	}
	return S_OK;
}

HRESULT InputManager::EndUIElement(DWORD dwUIElementId)
{
	std::cout << "Enter the EndUIElement!" << std::endl;
	OnCandidateReceived(-1, m_candidates, 0); //置空候选词
	ITfDocumentMgr* pDocMgr = nullptr;
	ITfContext* pContex = nullptr;
	ITfContextView* pContexView = nullptr;

	HWND hActiveHwnd = nullptr;

	if (SUCCEEDED(m_pThreadMgr->GetFocus(&pDocMgr)))
	{
		if (pDocMgr != nullptr && SUCCEEDED(pDocMgr->GetTop(&pContex)))
		{
			if (SUCCEEDED(pContex->GetActiveView(&pContexView)))
			{
				pContexView->GetWnd(&hActiveHwnd);
				pContexView->Release();
			}
			if (pContex != nullptr)
				pContex->Release();
		}
		if (pDocMgr != nullptr)
			pDocMgr->Release();
	}

	if (nullptr != hActiveHwnd)
	{
		SendMessageW(hActiveHwnd, WM_IME_NOTIFY, IMN_CLOSECANDIDATE, 0);
	}
	return S_OK;
}

