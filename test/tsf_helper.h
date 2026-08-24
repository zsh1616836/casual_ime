#ifndef INPUT_HELPER_H
#define INPUT_HELPER_H

#include <Windows.h>
#include <msctf.h>

class InputManager : public ITfUIElementSink,
                     public ITfTextEditSink,
                     public ITfThreadMgrEventSink
{
private:
	ITfThreadMgrEx* m_pThreadMgr;
	TfClientId m_tf_client_id;
	DWORD m_ObjRefCount;
	ITfContext* m_pic;
	//HWND m_hwnd;


	DWORD dwUIElementSinkCookie;
	DWORD dwThreadMgrEventCookie;
	DWORD dwTextEditSink;
public:
	InputManager();
	virtual ~InputManager();

	bool Initialize();

	bool Dispose();

	/*ITfThreadMgrEventSink*/
	HRESULT QueryInterface(const IID& riid, void** ppvObject) override;
	ULONG AddRef() override;
	ULONG Release() override;
	HRESULT OnInitDocumentMgr(ITfDocumentMgr* pdim) override;
	HRESULT OnUninitDocumentMgr(ITfDocumentMgr* pdim) override;
	HRESULT OnSetFocus(ITfDocumentMgr* pdimFocus, ITfDocumentMgr* pdimPrevFocus) override;
	HRESULT OnPushContext(ITfContext* pic) override;
	HRESULT OnPopContext(ITfContext* pic) override;

	/*ITfTextEditSink*/
	HRESULT OnEndEdit(ITfContext* pic, TfEditCookie ecReadOnly, ITfEditRecord* pEditRecord) override;

	/*ITfUIElementSink*/
	HRESULT BeginUIElement(DWORD dwUIElementId, BOOL* pbShow) override;
	HRESULT UpdateUIElement(DWORD dwUIElementId) override;
	HRESULT EndUIElement(DWORD dwUIElementId) override;
};
#endif
