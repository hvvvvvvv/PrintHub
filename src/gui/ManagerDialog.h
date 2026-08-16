#pragma once

#include <ServiceConfiguration.hpp>

#include <afxcmn.h>
#include <afxdialogex.h>
#include <afxdlgs.h>
#include <afxwin.h>

#include <optional>
#include <unordered_map>

class CManagerDialog final : public CDialogEx {
public:
    CManagerDialog();

private:
    enum class Action { InstallOrUpdate, Start, Stop, Restart, Uninstall };
    struct ServiceSnapshot {
        bool installed = false;
        bool running = false;
        std::wstring imagePath;
        DWORD error = ERROR_SUCCESS;
    };
    struct WorkItem;
    struct WorkResult;

    static constexpr UINT kOperationCompletedMessage = WM_APP + 1;
    static constexpr wchar_t kServiceName[] = L"PrintHub";

    BOOL OnInitDialog() override;
    void OnCancel() override;
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* info);
    afx_msg void OnSize(UINT type, int width, int height);
    afx_msg void OnBrowseService();
    afx_msg void OnRefresh();
    afx_msg void OnAddRaw();
    afx_msg void OnRemoveRaw();
    afx_msg void OnAddLpdQueue();
    afx_msg void OnRemoveLpdQueue();
    afx_msg void OnShowRawMapping();
    afx_msg void OnShowLpdMapping();
    afx_msg void OnInstallOrUpdate();
    afx_msg void OnStart();
    afx_msg void OnStop();
    afx_msg void OnRestart();
    afx_msg void OnUninstall();
    afx_msg void OnOpenLogs();
    afx_msg void OnConfigurationChanged();
    afx_msg LRESULT OnOperationCompleted(WPARAM, LPARAM result);

    static UINT __cdecl RunOperationThread(LPVOID parameter);
    static ServiceSnapshot QueryService();
    static bool WaitForServiceDeletion();
    static WorkResult RunAction(const WorkItem& work);
    static WorkResult RunServiceCommand(const std::wstring& executable, const std::vector<std::wstring>& arguments);

    void PopulatePrinters();
    void UpdateMappingTab();
    void RefreshServiceStatus(bool loadConfiguration);
    void LoadConfiguration(const ServiceConfiguration& configuration);
    std::optional<ServiceConfiguration> ReadConfiguration(bool showErrors);
    void UpdatePreview();
    void CaptureBaseLayout();
    void LayoutControls(int width, int height);
    CSize MinimumClientSize() const;
    void StartAction(Action action);
    void SetBusy(bool busy);
    static std::wstring DefaultServicePath();
    static std::wstring ToWide(const std::string& value);
    static std::string ToNarrow(const std::wstring& value);
    static CString FormatWin32Error(DWORD error);

    CEdit m_servicePath;
    CStatic m_status;
    CEdit m_listenAddress;
    CEdit m_rawThreads;
    CComboBox m_printerName;
    CEdit m_rawPort;
    CListCtrl m_rawList;
    CButton m_rawMappingPage;
    CButton m_lpdMappingPage;
    CEdit m_lpdQueueName;
    CComboBox m_lpdPrinterName;
    CListCtrl m_lpdQueueList;
    CEdit m_lpdPort;
    CEdit m_lpdThreads;
    CButton m_snmpReject;
    CComboBox m_logLevel;
    CEdit m_preview;
    std::vector<RawServerConfiguration> m_rawServers;
    std::vector<LpdQueueMapping> m_lpdQueues;
    bool m_unknownExistingConfiguration = false;
    bool m_busy = false;
    bool m_layoutReady = false;
    CSize m_baseClientSize{};
    std::unordered_map<UINT, CRect> m_baseLayout;

    DECLARE_MESSAGE_MAP()
};
