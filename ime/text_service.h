#pragma once

#include "ime_dictionary.h"
#include "broker_client.h"
#include "candidate_form.h"
#include "candidate_ui_element.h"
#include "status_window.h"
#include <ctffunc.h>
#include <msctf.h>
#include <filesystem>
#include <unordered_map>

class search_candidate_provider;

class text_service : public ITfTextInputProcessorEx,
                     public ITfThreadMgrEventSink,
                     public ITfKeyEventSink,
                     public ITfCompositionSink,
                     public ITfTextLayoutSink,
                     public ITfFunctionProvider,
                     public ITfCompartmentEventSink
{
public:
    text_service();
    virtual ~text_service();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppvObj) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // ITfTextInputProcessor
    STDMETHODIMP Activate(ITfThreadMgr *pThreadMgr, TfClientId tfClientId) override;
    STDMETHODIMP Deactivate() override;

    // ITfTextInputProcessorEx
    STDMETHODIMP ActivateEx(ITfThreadMgr *pThreadMgr, TfClientId tfClientId, DWORD dwFlags) override;

    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr *pDocMgr) override;
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr *pDocMgr) override;
    STDMETHODIMP OnSetFocus(ITfDocumentMgr *pDocMgrFocus, ITfDocumentMgr *pDocMgrPrevFocus) override;
    STDMETHODIMP OnPushContext(ITfContext *pContext) override;
    STDMETHODIMP OnPopContext(ITfContext *pContext) override;

    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL fForeground) override;
    STDMETHODIMP OnTestKeyDown(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten) override;
    STDMETHODIMP OnKeyDown(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten) override;
    STDMETHODIMP OnKeyUp(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext *pContext, REFGUID rguid, BOOL *pfEaten) override;
    
    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition *pComposition) override;

    // ITfTextLayoutSink
    STDMETHODIMP OnLayoutChange(ITfContext* pContext, TfLayoutCode lcode, ITfContextView* pView) override;

    // ITfFunctionProvider
    STDMETHODIMP GetType(GUID* pguid) override;
    STDMETHODIMP GetDescription(BSTR* pbstrDesc) override;
    STDMETHODIMP GetFunction(REFGUID rguid, REFIID riid, IUnknown** ppunk) override;

    // ITfCompartmentEventSink
    STDMETHODIMP OnChange(REFGUID rguid) override;

private:
    enum class pending_candidate_action
    {
        none,
        space,
        number,
    };

    friend class composition_edit_session;
    friend class search_candidate_provider;
    static LRESULT CALLBACK CreateWordWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    HRESULT ActivateInternal(ITfThreadMgr *pThreadMgr, TfClientId tfClientId, DWORD dwFlags);
    BOOL InitThreadMgrEventSink();
    void UninitThreadMgrEventSink();
    BOOL InitKeyEventSink();
    void UninitKeyEventSink();
    BOOL InitFunctionProviderSink();
    void UninitFunctionProviderSink();
    BOOL InitInputModeCompartment();
    void UninitInputModeCompartment();
    void SyncInputModeCompartment();
    void SendBrokerContextSnapshot(HWND view_hwnd);
    void RefreshBrokerContextForCurrentFocus();
    void SyncBrokerCandidateState(const RECT* anchor_override = nullptr);
    void SyncBrokerStatusState();
    void OnBrokerConnectionChanged(bool connected);
    void OnBrokerUiAction(zime::broker_protocol::ui_action_type action,
                          std::uint32_t value,
                          POINT screen_point);
    void OnBrokerStorageResult(
        zime::broker_protocol::storage_operation operation,
        std::uint64_t request_id,
        bool success,
        const std::wstring& error);
    void OnBrokerConfigState(
        const zime::broker_protocol::config_state& state);
    void OnBrokerCandidateResult(
        std::uint64_t generation,
        const std::wstring& code,
        const std::vector<std::wstring>& candidates,
        const std::vector<std::wstring>& view_texts,
        const std::vector<bool>& pinyin_flags,
        std::uint32_t result_flags,
        std::uint64_t config_revision);
    void RequestBrokerCandidates(bool apply_auto_commit = false);
    bool ResolveBrokerCandidatesSynchronously(bool apply_auto_commit);
    void RefreshCandidatesAfterStorageMutation();
    HRESULT BuildSearchCandidates(BSTR query, std::vector<std::wstring>& candidates);
    bool AdviseTextLayoutSink(ITfContext* pContext);
    void UnadviseTextLayoutSink();
    void RefreshTextLayoutSink(ITfDocumentMgr* pDocMgrFocus);
    void RefreshTextLayoutSinkForCurrentFocus();
    
    void HandleCharacter(ITfContext *pContext, WCHAR wch);
    void HandleBackspace(ITfContext *pContext);
    void HandleSpace(ITfContext *pContext);
    void HandleNumber(ITfContext *pContext, int num);
    void CommitFirstCandidateOrComposition(ITfContext *pContext);
    [[nodiscard]] bool IsCurrentFirstCandidatePinyin();
    void ShowCreateWordWindow();
    void OnConfirmCreateWord(HWND hWnd);
    void ExportRawDictionary();
    void OnCandidateClicked(int candidate_index);
    void OnCandidateContextMenu(int candidate_index, POINT screen_point);
    void OnCandidateContextCommand(int candidate_index, bool delete_candidate);
    std::wstring ResolveCandidateCodeForContextMenu(const std::wstring& code_snapshot,
                                                    const std::wstring& candidate_text,
                                                    const std::wstring& display_text) const;
    void RecordCandidateSelection(const std::wstring& code_snapshot,
                                  const std::wstring& candidate_text,
                                  const std::wstring& display_text);
    void CommitCompositionCodeAndClear(ITfContext* pContext);
    void InsertRawText(ITfContext *pContext, const std::wstring& text);
    void InsertText(ITfContext *pContext, const std::wstring& text);
    void UpdateCompositionInContext(ITfContext* pContext);
    void CancelCompositionInContext(ITfContext* pContext);
    void ClearComposition();
    void ShowCandidates(ITfContext *pContext);
    void HideCandidates();
    void OnHostCandidateUiShowChanged(BOOL bShow);
    [[nodiscard]] bool ShouldShowOwnCandidateWindow() const;
    [[nodiscard]] bool ShouldShowBrokerCandidateWindow() const;
    [[nodiscard]] bool ShouldShowStatusWindow() const;
    [[nodiscard]] bool ShouldShowLocalStatusWindow() const;
    void SetCandidateSelectionAbsolute(UINT index);
    [[nodiscard]] int GetCandidateSelectionAbsolute() const;
    void FinalizeCurrentCandidateSelection();
    void AbortCurrentCandidateUi();
    BOOL HandleCandidateUiKeyDown(WPARAM wParam, LPARAM lParam);
    void FinalizeExactCompositionStringFromUi();
    void ApplyCandidateWindowVisibility();
    void EnsureCandidateWindow();
    void UpdateCandidateUIElement(ITfContext *pContext);
    void EndCandidateUIElement();
    void UpdateCandidateWindowPosition(ITfContext* pContext,
                                       bool asynchronous = false,
                                       bool confirms_layout = false);
    void UpdateCandidateWindowPositionFromRect(const RECT& rc);
    void RememberCandidateAnchorRect(const RECT& rc);
    void ClearCandidateAnchorRect();
    [[nodiscard]] bool TryGetCachedCandidateAnchorRect(RECT* rc_out) const;
    
    // 字符转换功能
    std::wstring ConvertToFullWidth(const std::wstring& text);
    std::wstring ConvertPunctuation(const std::wstring& text);
    std::wstring ProcessTextBeforeInsert(const std::wstring& text);

    ITfThreadMgr* m_pThreadMgr;
    LONG m_cRef;
    TfClientId m_tfClientId;
    DWORD m_dwThreadMgrEventSinkCookie;
    DWORD m_dwKeyEventSinkCookie;
    DWORD m_dwTextLayoutSinkCookie;
    ITfContext* m_pTextLayoutSinkContext;
    bool m_functionProviderAdvised;
    search_candidate_provider* m_searchCandidateProvider;
    ITfCompartment* m_inputModeCompartment;
    DWORD m_inputModeCompartmentSinkCookie;
    bool m_updatingInputModeCompartment;
    broker_client m_brokerClient;
    std::uint64_t m_pendingCreateWordRequest;
    bool m_hasBrokerConfig;
    std::uint64_t m_brokerConfigRevision;
    bool m_brokerCommitFirstOnNextCode;
    bool m_firstCandidateIsPinyin;
    std::wstring m_brokerCandidateCode;
    std::wstring m_pendingCodeCharacters;
    pending_candidate_action m_pendingCandidateAction;
    std::wstring m_pendingCandidateActionCode;
    int m_pendingCandidateNumber;
    std::unordered_map<std::uint64_t,
                       zime::broker_protocol::storage_operation>
        m_pendingCandidateStorageRequests;
    
    candidate_form m_candidateWindow;
    candidate_ui_element *m_candidateUIElement;
    DWORD m_candidateUIElementId;
    bool m_hostWantsCandidateWindow;
    bool m_uiElementOnlyMode;
    bool m_immersiveMode;
    HWND m_brokerViewHwnd;
    RECT m_lastCandidateAnchorRect;
    bool m_hasLastCandidateAnchorRect;
    DWORD m_lastCandidateAnchorTick;
    HWND m_lastCandidateAnchorOwner;
    bool m_candidatePositionEditPending;
    bool m_brokerCandidateLayoutReady;
    status_window m_statusWindow;
    HWND m_hCreateWordWnd;
    
    std::wstring m_compositionText; // composition string
    ITfComposition* m_pComposition;

    BOOL m_bInComposition;  // inputting

    BOOL m_bChineseMode;  // TRUE = 中文模式, FALSE = 英文模式

    BOOL m_bFullWidth;    // TRUE = 全角, FALSE = 半角

    BOOL m_bChinesePunctuation;  // TRUE = 中文标点, FALSE = 英文标点
    bool m_autoCommitFourCodeUnique; // 四码唯一时直接上屏
    bool m_commitFirstCandidateOnFifthCode; // 第5码时上屏首个候选词
    bool m_showUncommonCandidates; // 候选不常用字词
    bool m_replaceDotAfterDigit; // 数字后"。"替换为"."
    bool m_useEnglishPunctuationInChineseMode; // 中文模式时默认使用英文标点
    bool m_disableChineseDash; // Shift+- 不使用中文破折号
    ime_dict::candidate_sort_mode m_candidateSortMode; // 候选词排序方式
    int m_uiFontPercent; // UI 字体缩放（百分比）
    
    // Shift键状态跟踪
    bool m_bShiftPressed;       // Shift键是否被按下
    bool m_bOtherKeyPressed;    // 在Shift按下期间是否有其他键被按下
    bool m_bShiftPressedWithModifier; // Shift按下时是否伴随 Ctrl/Alt/Win
    bool m_lastInputWasDigit;   // 上一次按键输入是否为数字0-9（不含Shift+数字）
    bool m_inCandidateContextMenu;
    bool m_inStatusMenuPopup;
    DWORD m_statusWindowHideSuppressedUntilTick;
    
    void UpdateStatusWindow(bool sync_broker = true);
    void ShowStatusWindow();
    void OnStatusChanged(int status_type);
    void OnStatusWindowMoved(int x, int y);
    std::filesystem::path GetConfigPath() const;
    void SyncPunctuationModeWithLanguageMode();
    void LoadRuntimeConfig();
    void SaveRuntimeConfig();

    bool m_statusWindowPositionCustomized;
    bool m_statusWindowDesiredVisible;
    int m_statusWindowPosX;
    int m_statusWindowPosY;
};
