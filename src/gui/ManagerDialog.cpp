#include "ManagerDialog.h"
#include "resource.h"
#include <ManagerCommands.hpp>

#include <windows.h>
#include <shellapi.h>
#include <winspool.h>
#include <winsvc.h>

#include <filesystem>
#include <algorithm>
#include <memory>

struct CManagerDialog::WorkItem {
    Action action;
    std::wstring executable;
    ServiceConfiguration configuration;
    ServiceSnapshot snapshot;
    HWND targetWindow = nullptr;
    std::shared_ptr<OperationState> state;
};

struct CManagerDialog::WorkResult {
    bool success = false;
    CString details;
};

struct CManagerDialog::OperationState {
    bool cancelled = false;
    std::mutex mutex;
};

struct CManagerDialog::StartupLoadResult {
    std::vector<std::wstring> printers;
    DWORD printerError = ERROR_SUCCESS;
    ServiceSnapshot snapshot;
    std::optional<ServiceConfiguration> configuration;
    std::wstring executable;
    bool unknownConfiguration = false;
};

struct CManagerDialog::StartupLoadState {
    HWND targetWindow = nullptr;
    std::atomic<bool> cancelled = false;
    std::mutex mutex;
    std::unique_ptr<StartupLoadResult> result;
};

struct CManagerDialog::StartupLoadWorkItem {
    std::shared_ptr<StartupLoadState> state;
};

BEGIN_MESSAGE_MAP(CManagerDialog, CDialogEx)
    ON_WM_DESTROY()
    ON_WM_GETMINMAXINFO()
    ON_WM_SIZE()
    ON_BN_CLICKED(IDC_BROWSE_SERVICE, &CManagerDialog::OnBrowseService)
    ON_BN_CLICKED(IDC_REFRESH, &CManagerDialog::OnRefresh)
    ON_BN_CLICKED(IDC_ADD_RAW, &CManagerDialog::OnAddRaw)
    ON_BN_CLICKED(IDC_REMOVE_RAW, &CManagerDialog::OnRemoveRaw)
    ON_BN_CLICKED(IDC_ADD_LPD_QUEUE, &CManagerDialog::OnAddLpdQueue)
    ON_BN_CLICKED(IDC_REMOVE_LPD_QUEUE, &CManagerDialog::OnRemoveLpdQueue)
    ON_BN_CLICKED(IDC_INSTALL_UPDATE, &CManagerDialog::OnInstallOrUpdate)
    ON_BN_CLICKED(IDC_START_SERVICE, &CManagerDialog::OnStart)
    ON_BN_CLICKED(IDC_STOP_SERVICE, &CManagerDialog::OnStop)
    ON_BN_CLICKED(IDC_RESTART_SERVICE, &CManagerDialog::OnRestart)
    ON_BN_CLICKED(IDC_UNINSTALL_SERVICE, &CManagerDialog::OnUninstall)
    ON_BN_CLICKED(IDC_OPEN_LOGS, &CManagerDialog::OnOpenLogs)
    ON_BN_CLICKED(IDC_SNMP_REJECT, &CManagerDialog::OnConfigurationChanged)
    ON_EN_CHANGE(IDC_SERVICE_PATH, &CManagerDialog::OnConfigurationChanged)
    ON_EN_CHANGE(IDC_LISTEN_ADDRESS, &CManagerDialog::OnConfigurationChanged)
    ON_EN_CHANGE(IDC_RAW_THREADS, &CManagerDialog::OnConfigurationChanged)
    ON_EN_CHANGE(IDC_LPD_PORT, &CManagerDialog::OnConfigurationChanged)
    ON_EN_CHANGE(IDC_LPD_THREADS, &CManagerDialog::OnConfigurationChanged)
    ON_EN_CHANGE(IDC_MAX_PENDING_CLIENTS, &CManagerDialog::OnConfigurationChanged)
    ON_EN_CHANGE(IDC_LPD_MAX_CONTROL_BYTES, &CManagerDialog::OnConfigurationChanged)
    ON_CBN_SELCHANGE(IDC_LOG_LEVEL, &CManagerDialog::OnConfigurationChanged)
    ON_BN_CLICKED(IDC_RAW_MAPPING_PAGE, &CManagerDialog::OnShowRawMapping)
    ON_BN_CLICKED(IDC_LPD_MAPPING_PAGE, &CManagerDialog::OnShowLpdMapping)
    ON_MESSAGE(kOperationCompletedMessage, &CManagerDialog::OnOperationCompleted)
    ON_MESSAGE(kStartupLoadCompletedMessage, &CManagerDialog::OnStartupLoadCompleted)
END_MESSAGE_MAP()

CManagerDialog::CManagerDialog() : CDialogEx(IDD_MANAGER_DIALOG) {}

BOOL CManagerDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();
    m_servicePath.SubclassDlgItem(IDC_SERVICE_PATH, this);
    m_status.SubclassDlgItem(IDC_SERVICE_STATUS, this);
    m_listenAddress.SubclassDlgItem(IDC_LISTEN_ADDRESS, this);
    m_rawThreads.SubclassDlgItem(IDC_RAW_THREADS, this);
    m_printerName.SubclassDlgItem(IDC_PRINTER_NAME, this);
    m_rawPort.SubclassDlgItem(IDC_RAW_PORT, this);
    m_rawList.SubclassDlgItem(IDC_RAW_LIST, this);
    m_rawMappingPage.SubclassDlgItem(IDC_RAW_MAPPING_PAGE, this);
    m_lpdMappingPage.SubclassDlgItem(IDC_LPD_MAPPING_PAGE, this);
    m_lpdQueueName.SubclassDlgItem(IDC_LPD_QUEUE_NAME, this);
    m_lpdPrinterName.SubclassDlgItem(IDC_LPD_PRINTER_NAME, this);
    m_lpdQueueList.SubclassDlgItem(IDC_LPD_QUEUE_LIST, this);
    m_lpdPort.SubclassDlgItem(IDC_LPD_PORT, this);
    m_lpdThreads.SubclassDlgItem(IDC_LPD_THREADS, this);
    m_maxPendingClients.SubclassDlgItem(IDC_MAX_PENDING_CLIENTS, this);
    m_lpdMaxControlBytes.SubclassDlgItem(IDC_LPD_MAX_CONTROL_BYTES, this);
    m_snmpReject.SubclassDlgItem(IDC_SNMP_REJECT, this);
    m_logLevel.SubclassDlgItem(IDC_LOG_LEVEL, this);
    m_preview.SubclassDlgItem(IDC_COMMAND_PREVIEW, this);

    const HICON icon = AfxGetApp()->LoadIcon(IDI_MANAGER_APP);
    SetIcon(icon, TRUE);
    SetIcon(icon, FALSE);

    m_rawList.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    m_rawList.InsertColumn(0, L"打印机名称", LVCFMT_LEFT, 330);
    m_rawList.InsertColumn(1, L"监听端口", LVCFMT_RIGHT, 100);
    m_lpdQueueList.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    m_lpdQueueList.InsertColumn(0, L"队列名称", LVCFMT_LEFT, 150);
    m_lpdQueueList.InsertColumn(1, L"打印机名称", LVCFMT_LEFT, 280);
    m_rawMappingPage.SetCheck(BST_CHECKED);
    for (const auto* level : { L"trace", L"debug", L"info", L"warn", L"error", L"critical", L"off" }) {
        m_logLevel.AddString(level);
    }
    m_logLevel.SetCurSel(2);
    m_servicePath.SetWindowTextW(DefaultServicePath().c_str());
    m_listenAddress.SetWindowTextW(L"0.0.0.0");
    m_rawThreads.SetWindowTextW(L"1");
    m_rawPort.SetWindowTextW(L"9100");
    m_lpdPort.SetWindowTextW(L"515");
    m_lpdThreads.SetWindowTextW(L"1");
    m_maxPendingClients.SetWindowTextW(std::to_wstring(ServiceConfiguration::kDefaultMaxPendingClients).c_str());
    m_lpdMaxControlBytes.SetWindowTextW(std::to_wstring(ServiceConfiguration::kDefaultLpdMaxControlFileBytes).c_str());
    UpdateMappingTab();
    UpdatePreview();
    CaptureBaseLayout();
    m_layoutReady = true;
    CRect client;
    GetClientRect(&client);
    LayoutControls(client.Width(), client.Height());
    m_status.SetWindowTextW(L"服务状态：正在加载打印机和服务配置...");
    StartStartupLoad();
    return TRUE;
}

void CManagerDialog::OnCancel() {
    if (m_busy) return;
    PrepareForClose();
    CDialogEx::OnCancel();
}

void CManagerDialog::OnDestroy() {
    CancelStartupLoad();
    CancelOperation();
    CDialogEx::OnDestroy();
}

void CManagerDialog::OnGetMinMaxInfo(MINMAXINFO* info) {
    CDialogEx::OnGetMinMaxInfo(info);
    const auto minimum = MinimumClientSize();
    RECT window{ 0, 0, minimum.cx, minimum.cy };
    AdjustWindowRectEx(&window, static_cast<DWORD>(GetStyle()), FALSE, static_cast<DWORD>(GetExStyle()));
    info->ptMinTrackSize.x = window.right - window.left;
    info->ptMinTrackSize.y = window.bottom - window.top;
}

void CManagerDialog::OnSize(UINT type, int width, int height) {
    CDialogEx::OnSize(type, width, height);
    if (m_layoutReady && type != SIZE_MINIMIZED) LayoutControls(width, height);
}

void CManagerDialog::OnBrowseService() {
    CFileDialog dialog(TRUE, L"exe", nullptr, OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
        L"PrintHub 服务程序 (PrintHub.exe)|PrintHub.exe|可执行文件 (*.exe)|*.exe||", this);
    if (dialog.DoModal() == IDOK) {
        m_servicePath.SetWindowTextW(dialog.GetPathName());
        UpdatePreview();
    }
}

void CManagerDialog::OnRefresh() {
    CancelStartupLoad();
    RefreshServiceStatus(true);
}

void CManagerDialog::OnAddRaw() {
    CString printer;
    CString portText;
    m_printerName.GetWindowTextW(printer);
    m_rawPort.GetWindowTextW(portText);
    printer.Trim();
    portText.Trim();
    if (printer.IsEmpty() || portText.IsEmpty()) {
        AfxMessageBox(L"请输入打印机名称和 RAW 端口。", MB_ICONWARNING);
        return;
    }
    try {
        std::vector<std::string> candidateArguments{ "--raw-server", ToNarrow(printer.GetString()) + ":" + ToNarrow(portText.GetString()) };
        const auto candidate = ServiceConfiguration::Parse(candidateArguments);
        if (!IsPrinterAvailable(printer.GetString())) {
            AfxMessageBox(L"找不到指定的本机或连接打印机。请从下拉列表选择，或确认名称正确。", MB_ICONWARNING);
            return;
        }
        const auto& entry = candidate.rawServers.front();
        for (const auto& existing : m_rawServers) {
            if (existing.port == entry.port) {
                AfxMessageBox(L"RAW 监听端口不能重复。", MB_ICONWARNING);
                return;
            }
        }
        m_rawServers.push_back(entry);
        const int row = m_rawList.InsertItem(m_rawList.GetItemCount(), ToWide(entry.printerName).c_str());
        m_rawList.SetItemText(row, 1, std::to_wstring(entry.port).c_str());
        UpdatePreview();
    } catch (const std::exception& error) {
        AfxMessageBox((CString(L"RAW 映射无效：") + ToWide(error.what()).c_str()), MB_ICONWARNING);
    }
}

void CManagerDialog::OnRemoveRaw() {
    const int row = m_rawList.GetNextItem(-1, LVNI_SELECTED);
    if (row < 0) return;
    m_rawServers.erase(m_rawServers.begin() + row);
    m_rawList.DeleteItem(row);
    UpdatePreview();
}

void CManagerDialog::OnAddLpdQueue() {
    CString queue;
    CString printer;
    m_lpdQueueName.GetWindowTextW(queue);
    m_lpdPrinterName.GetWindowTextW(printer);
    queue.Trim();
    printer.Trim();
    if (queue.IsEmpty() || printer.IsEmpty()) {
        AfxMessageBox(L"请输入队列名和打印机名称。", MB_ICONWARNING);
        return;
    }
    try {
        const auto queueName = ToNarrow(queue.GetString());
        const auto printerName = ToNarrow(printer.GetString());
        const auto candidate = ServiceConfiguration::Parse({ "--lpd-queue", queueName + "=" + printerName });
        if (ResolveLpdQueuePrinter(m_lpdQueues, queueName).has_value()) {
            AfxMessageBox(L"LPD 队列名不能重复（不区分大小写）。", MB_ICONWARNING);
            return;
        }
        if (!IsPrinterAvailable(printer.GetString())) {
            AfxMessageBox(L"找不到指定的本机或连接打印机。请从下拉列表选择，或确认名称正确。", MB_ICONWARNING);
            return;
        }
        const auto& entry = candidate.lpdQueues.front();
        m_lpdQueues.push_back(entry);
        const int row = m_lpdQueueList.InsertItem(m_lpdQueueList.GetItemCount(), ToWide(entry.queueName).c_str());
        m_lpdQueueList.SetItemText(row, 1, ToWide(entry.printerName).c_str());
        UpdatePreview();
    } catch (const std::exception& error) {
        AfxMessageBox((CString(L"LPD 队列映射无效：") + ToWide(error.what()).c_str()), MB_ICONWARNING);
    }
}

void CManagerDialog::OnRemoveLpdQueue() {
    const int row = m_lpdQueueList.GetNextItem(-1, LVNI_SELECTED);
    if (row < 0) return;
    m_lpdQueues.erase(m_lpdQueues.begin() + row);
    m_lpdQueueList.DeleteItem(row);
    UpdatePreview();
}

void CManagerDialog::OnShowRawMapping() {
    m_rawMappingPage.SetCheck(BST_CHECKED);
    m_lpdMappingPage.SetCheck(BST_UNCHECKED);
    UpdateMappingTab();
}

void CManagerDialog::OnShowLpdMapping() {
    m_rawMappingPage.SetCheck(BST_UNCHECKED);
    m_lpdMappingPage.SetCheck(BST_CHECKED);
    UpdateMappingTab();
}

void CManagerDialog::OnInstallOrUpdate() { StartAction(Action::InstallOrUpdate); }
void CManagerDialog::OnStart() { StartAction(Action::Start); }
void CManagerDialog::OnStop() { StartAction(Action::Stop); }
void CManagerDialog::OnRestart() { StartAction(Action::Restart); }
void CManagerDialog::OnUninstall() { StartAction(Action::Uninstall); }

void CManagerDialog::OnOpenLogs() {
    CString path;
    m_servicePath.GetWindowTextW(path);
    const auto logPath = std::filesystem::path(std::wstring(path)).parent_path() / L"logs";
    if (!std::filesystem::is_directory(logPath)) {
        AfxMessageBox(L"日志目录尚不存在。请先启动服务并等待其写入日志。", MB_ICONINFORMATION);
        return;
    }
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(m_hWnd, L"open", logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        AfxMessageBox(L"无法打开日志目录。", MB_ICONERROR);
    }
}

void CManagerDialog::OnConfigurationChanged() {
    UpdatePreview();
}

bool CManagerDialog::IsPrinterAvailable(const std::wstring& printerName) {
    HANDLE handle = nullptr;
    if (!OpenPrinterW(const_cast<LPWSTR>(printerName.c_str()), &handle, nullptr)) return false;
    ClosePrinter(handle);
    return true;
}

std::vector<std::wstring> CManagerDialog::EnumeratePrinters(DWORD& error) {
    error = ERROR_SUCCESS;
    DWORD bytes = 0;
    DWORD count = 0;
    if (EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2, nullptr, 0, &bytes, &count)) {
        return {};
    }
    const auto firstError = GetLastError();
    if (firstError != ERROR_INSUFFICIENT_BUFFER) {
        error = firstError;
        return {};
    }
    if (bytes == 0) return {};
    std::vector<BYTE> buffer(bytes);
    if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2, buffer.data(), bytes, &bytes, &count)) {
        error = GetLastError();
        return {};
    }
    std::vector<std::wstring> result;
    auto* printers = reinterpret_cast<PRINTER_INFO_2W*>(buffer.data());
    for (DWORD index = 0; index < count; ++index) {
        if (printers[index].pPrinterName) {
            result.emplace_back(printers[index].pPrinterName);
        }
    }
    return result;
}

void CManagerDialog::PopulatePrinters(const std::vector<std::wstring>& printers) {
    for (const auto& printer : printers) {
        if (m_printerName.FindStringExact(-1, printer.c_str()) == CB_ERR) {
            m_printerName.AddString(printer.c_str());
        }
        if (m_lpdPrinterName.FindStringExact(-1, printer.c_str()) == CB_ERR) {
            m_lpdPrinterName.AddString(printer.c_str());
        }
    }
}

bool CManagerDialog::ParseServiceImagePath(const std::wstring& imagePath, std::wstring& executable,
    ServiceConfiguration& configuration) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(imagePath.c_str(), &argc);
    if (!argv || argc < 1) {
        if (argv) LocalFree(argv);
        return false;
    }

    // Keep argument errors distinct from an unreadable command line for the UI.
    try {
        std::vector<std::string> arguments;
        for (int index = 1; index < argc; ++index) arguments.push_back(ToNarrow(argv[index]));
        configuration = ServiceConfiguration::Parse(arguments);
        executable = argv[0];
    } catch (...) {
        LocalFree(argv);
        throw;
    }
    LocalFree(argv);
    return true;
}

CManagerDialog::StartupLoadResult CManagerDialog::LoadStartupData() {
    StartupLoadResult result;
    result.printers = EnumeratePrinters(result.printerError);
    result.snapshot = QueryService();
    if (result.snapshot.error != ERROR_SUCCESS || !result.snapshot.installed) return result;

    try {
        ServiceConfiguration configuration;
        if (ParseServiceImagePath(result.snapshot.imagePath, result.executable, configuration)) {
            result.configuration = std::move(configuration);
        } else {
            result.unknownConfiguration = true;
        }
    } catch (const std::exception&) {
        result.unknownConfiguration = true;
    }
    return result;
}

void CManagerDialog::StartStartupLoad() {
    m_startupLoadState = std::make_shared<StartupLoadState>();
    m_startupLoadState->targetWindow = GetSafeHwnd();
    auto* item = new StartupLoadWorkItem{ m_startupLoadState };
    if (!AfxBeginThread(RunStartupLoadThread, item)) {
        delete item;
        m_startupLoadState.reset();
        m_status.SetWindowTextW(L"服务状态：无法创建后台加载线程");
    }
}

void CManagerDialog::CancelStartupLoad() {
    if (!m_startupLoadState) return;
    m_startupLoadState->cancelled.store(true);
    std::lock_guard<std::mutex> lock(m_startupLoadState->mutex);
    m_startupLoadState->result.reset();
}

void CManagerDialog::CancelOperation() {
    if (!m_operationState) return;
    {
        std::lock_guard<std::mutex> lock(m_operationState->mutex);
        m_operationState->cancelled = true;
    }
    MSG message{};
    while (::PeekMessageW(&message, GetSafeHwnd(), kOperationCompletedMessage,
        kOperationCompletedMessage, PM_REMOVE)) {
        delete reinterpret_cast<WorkResult*>(message.lParam);
    }
}

void CManagerDialog::PrepareForClose() {
    if (m_controlsDetached) return;
    m_controlsDetached = true;
    CancelStartupLoad();
    CancelOperation();
    DetachControlWrappers();
}

void CManagerDialog::DetachControlWrappers() {
    for (CWnd* control : { static_cast<CWnd*>(&m_servicePath), static_cast<CWnd*>(&m_status), static_cast<CWnd*>(&m_listenAddress),
        static_cast<CWnd*>(&m_rawThreads), static_cast<CWnd*>(&m_printerName), static_cast<CWnd*>(&m_rawPort),
        static_cast<CWnd*>(&m_rawList), static_cast<CWnd*>(&m_rawMappingPage), static_cast<CWnd*>(&m_lpdMappingPage),
        static_cast<CWnd*>(&m_lpdQueueName), static_cast<CWnd*>(&m_lpdPrinterName), static_cast<CWnd*>(&m_lpdQueueList),
        static_cast<CWnd*>(&m_lpdPort), static_cast<CWnd*>(&m_lpdThreads), static_cast<CWnd*>(&m_maxPendingClients),
        static_cast<CWnd*>(&m_lpdMaxControlBytes), static_cast<CWnd*>(&m_snmpReject), static_cast<CWnd*>(&m_logLevel),
        static_cast<CWnd*>(&m_preview) }) {
        const HWND handle = control->GetSafeHwnd();
        if (!handle) continue;
        if (::IsWindow(handle)) {
            control->UnsubclassWindow();
        } else {
            control->Detach();
        }
    }
}

void CManagerDialog::ApplyStartupLoadResult(const StartupLoadResult& result) {
    PopulatePrinters(result.printers);
    if (result.snapshot.error != ERROR_SUCCESS) {
        m_status.SetWindowTextW((CString(L"服务状态查询失败：") + FormatWin32Error(result.snapshot.error)).GetString());
        return;
    }
    if (!result.snapshot.installed) {
        m_status.SetWindowTextW(L"服务状态：未安装");
        m_unknownExistingConfiguration = false;
    } else if (result.unknownConfiguration || !result.configuration.has_value()) {
        m_unknownExistingConfiguration = true;
        m_status.SetWindowTextW(L"服务状态：已安装；配置包含未识别参数，更新前需确认覆盖");
    } else {
        m_status.SetWindowTextW(result.snapshot.running ? L"服务状态：已安装，正在运行" : L"服务状态：已安装，已停止");
        LoadConfiguration(*result.configuration);
        m_servicePath.SetWindowTextW(result.executable.c_str());
        m_unknownExistingConfiguration = false;
    }
    if (result.printerError != ERROR_SUCCESS) {
        CString currentStatus;
        m_status.GetWindowTextW(currentStatus);
        m_status.SetWindowTextW((currentStatus + L"；打印机列表加载失败：" + FormatWin32Error(result.printerError)).GetString());
    }
}

void CManagerDialog::UpdateMappingTab() {
    const bool rawVisible = m_rawMappingPage.GetCheck() == BST_CHECKED;
    const auto show = [this](UINT identifier, bool visible) {
        if (CWnd* control = GetDlgItem(identifier)) control->ShowWindow(visible ? SW_SHOW : SW_HIDE);
    };
    for (const auto identifier : { IDC_RAW_PRINTER_LABEL, IDC_PRINTER_NAME, IDC_RAW_PORT_LABEL, IDC_RAW_PORT,
        IDC_ADD_RAW, IDC_REMOVE_RAW, IDC_RAW_LIST }) {
        show(identifier, rawVisible);
    }
    for (const auto identifier : { IDC_LPD_QUEUE_LABEL, IDC_LPD_QUEUE_NAME, IDC_LPD_PRINTER_LABEL, IDC_LPD_PRINTER_NAME,
        IDC_ADD_LPD_QUEUE, IDC_REMOVE_LPD_QUEUE, IDC_LPD_QUEUE_LIST }) {
        show(identifier, !rawVisible);
    }
}

void CManagerDialog::RefreshServiceStatus(bool loadConfiguration) {
    const auto snapshot = QueryService();
    if (snapshot.error != ERROR_SUCCESS) {
        m_status.SetWindowTextW((CString(L"服务状态查询失败：") + FormatWin32Error(snapshot.error)).GetString());
        return;
    }
    if (!snapshot.installed) {
        m_status.SetWindowTextW(L"服务状态：未安装");
        m_unknownExistingConfiguration = false;
        return;
    }
    m_status.SetWindowTextW(snapshot.running ? L"服务状态：已安装，正在运行" : L"服务状态：已安装，已停止");
    if (!loadConfiguration) return;

    try {
        ServiceConfiguration configuration;
        std::wstring executable;
        if (!ParseServiceImagePath(snapshot.imagePath, executable, configuration)) {
            m_unknownExistingConfiguration = true;
            m_status.SetWindowTextW(L"服务状态：已安装；无法读取现有配置，更新前需确认覆盖");
            return;
        }
        LoadConfiguration(configuration);
        m_servicePath.SetWindowTextW(executable.c_str());
        m_unknownExistingConfiguration = false;
    } catch (const std::exception&) {
        m_unknownExistingConfiguration = true;
        m_status.SetWindowTextW(L"服务状态：已安装；配置包含未识别参数，更新前需确认覆盖");
    }
}

void CManagerDialog::LoadConfiguration(const ServiceConfiguration& configuration) {
    m_rawServers = configuration.rawServers;
    m_rawList.DeleteAllItems();
    for (const auto& entry : m_rawServers) {
        const int row = m_rawList.InsertItem(m_rawList.GetItemCount(), ToWide(entry.printerName).c_str());
        m_rawList.SetItemText(row, 1, std::to_wstring(entry.port).c_str());
    }
    m_lpdQueues = configuration.lpdQueues;
    m_lpdQueueList.DeleteAllItems();
    for (const auto& entry : m_lpdQueues) {
        const int row = m_lpdQueueList.InsertItem(m_lpdQueueList.GetItemCount(), ToWide(entry.queueName).c_str());
        m_lpdQueueList.SetItemText(row, 1, ToWide(entry.printerName).c_str());
    }
    m_listenAddress.SetWindowTextW(ToWide(configuration.listenAddress).c_str());
    m_rawThreads.SetWindowTextW(std::to_wstring(configuration.rawServerMinThreads).c_str());
    m_lpdPort.SetWindowTextW(std::to_wstring(configuration.lpdPort).c_str());
    m_lpdThreads.SetWindowTextW(std::to_wstring(configuration.lpdMinThreads).c_str());
    m_maxPendingClients.SetWindowTextW(std::to_wstring(configuration.maxPendingClients).c_str());
    m_lpdMaxControlBytes.SetWindowTextW(std::to_wstring(configuration.lpdMaxControlFileBytes).c_str());
    m_snmpReject.SetCheck(configuration.snmpReject ? BST_CHECKED : BST_UNCHECKED);
    m_logLevel.SelectString(-1, ToWide(ServiceConfiguration::LogLevelToString(configuration.logLevel)).c_str());
    UpdatePreview();
}

std::optional<ServiceConfiguration> CManagerDialog::ReadConfiguration(bool showErrors) {
    try {
        CString listenAddress, rawThreads, lpdPort, lpdThreads, maxPendingClients, lpdMaxControlBytes;
        m_listenAddress.GetWindowTextW(listenAddress);
        m_rawThreads.GetWindowTextW(rawThreads);
        m_lpdPort.GetWindowTextW(lpdPort);
        m_lpdThreads.GetWindowTextW(lpdThreads);
        m_maxPendingClients.GetWindowTextW(maxPendingClients);
        m_lpdMaxControlBytes.GetWindowTextW(lpdMaxControlBytes);
        CString level;
        const int levelIndex = m_logLevel.GetCurSel();
        if (levelIndex == CB_ERR) throw std::invalid_argument("请选择日志级别");
        m_logLevel.GetLBText(levelIndex, level);
        std::vector<std::string> arguments;
        for (const auto& entry : m_rawServers) {
            arguments.push_back("--raw-server");
            arguments.push_back(entry.printerName + ":" + std::to_string(entry.port));
        }
        for (const auto& entry : m_lpdQueues) {
            arguments.push_back("--lpd-queue");
            arguments.push_back(entry.queueName + "=" + entry.printerName);
        }
        arguments.insert(arguments.end(), { "--raw-server-min-threads", ToNarrow(rawThreads.GetString()), "--max-pending-clients", ToNarrow(maxPendingClients.GetString()),
            "--lpd-port", ToNarrow(lpdPort.GetString()), "--lpd-max-control-file-bytes", ToNarrow(lpdMaxControlBytes.GetString()),
            "--listen-address", ToNarrow(listenAddress.GetString()), "--lpd-min-threads", ToNarrow(lpdThreads.GetString()), "--log-level", ToNarrow(level.GetString()) });
        if (m_snmpReject.GetCheck() == BST_CHECKED) arguments.push_back("--snmp-reject");
        return ServiceConfiguration::Parse(arguments);
    } catch (const std::exception& error) {
        if (showErrors) AfxMessageBox((CString(L"配置无效：") + ToWide(error.what()).c_str()), MB_ICONWARNING);
        return std::nullopt;
    }
}

void CManagerDialog::UpdatePreview() {
    const auto configuration = ReadConfiguration(false);
    if (!configuration) {
        m_preview.SetWindowTextW(L"请完成有效配置后显示命令参数。");
        return;
    }
    CString servicePath;
    m_servicePath.GetWindowTextW(servicePath);
    std::vector<std::wstring> command{ servicePath.IsEmpty() ? DefaultServicePath() : std::wstring(servicePath.GetString()) };
    for (const auto& argument : BuildServiceManagerArguments(*configuration, ServiceManagerAction::Install,
        ServiceConfigurationArgumentFormat::Explicit)) {
        command.push_back(ToWide(argument));
    }
    m_preview.SetWindowTextW(BuildWindowsCommandLine(command).c_str());
}

void CManagerDialog::CaptureBaseLayout() {
    m_baseLayout.clear();
    for (const auto identifier : { IDC_SERVICE_PATH, IDC_BROWSE_SERVICE, IDC_REFRESH, IDC_SERVICE_STATUS,
        IDC_PROTOCOL_GROUP, IDC_RAW_GROUP, IDC_RAW_LIST, IDC_LPD_QUEUE_LIST,
        IDC_COMMAND_GROUP, IDC_COMMAND_PREVIEW,
        IDC_INSTALL_UPDATE, IDC_START_SERVICE, IDC_STOP_SERVICE, IDC_RESTART_SERVICE, IDC_UNINSTALL_SERVICE,
        IDC_OPEN_LOGS, IDC_OPERATION_HINT, IDCANCEL }) {
        CWnd* control = GetDlgItem(identifier);
        if (!control) continue;
        CRect rectangle;
        control->GetWindowRect(&rectangle);
        ScreenToClient(&rectangle);
        m_baseLayout.emplace(identifier, rectangle);
    }
    CRect client;
    GetClientRect(&client);
    m_baseClientSize = client.Size();
}

CSize CManagerDialog::MinimumClientSize() const {
    RECT units{ 0, 0, 500, 358 };
    const_cast<CManagerDialog*>(this)->MapDialogRect(&units);
    return { units.right, units.bottom };
}

void CManagerDialog::LayoutControls(int width, int height) {
    if (m_baseLayout.empty()) return;
    const int horizontalExtra = (std::max)(0, width - static_cast<int>(m_baseClientSize.cx));
    const int verticalExtra = (std::max)(0, height - static_cast<int>(m_baseClientSize.cy));
    const int mappingExtra = verticalExtra * 2 / 3;
    const int previewExtra = verticalExtra - mappingExtra;
    const auto move = [this](UINT identifier, int offsetX, int offsetY, int widthExtra = 0, int heightExtra = 0) {
        const auto iterator = m_baseLayout.find(identifier);
        if (iterator == m_baseLayout.end()) return;
        CRect rectangle = iterator->second;
        rectangle.OffsetRect(offsetX, offsetY);
        rectangle.right += widthExtra;
        rectangle.bottom += heightExtra;
        if (CWnd* control = GetDlgItem(identifier)) {
            control->SetWindowPos(nullptr, rectangle.left, rectangle.top, rectangle.Width(), rectangle.Height(),
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
    };

    move(IDC_SERVICE_PATH, 0, 0, horizontalExtra);
    move(IDC_BROWSE_SERVICE, horizontalExtra, 0);
    move(IDC_REFRESH, horizontalExtra, 0);
    move(IDC_SERVICE_STATUS, 0, 0, horizontalExtra);
    move(IDC_PROTOCOL_GROUP, 0, 0, horizontalExtra);
    move(IDC_RAW_GROUP, 0, 0, horizontalExtra, mappingExtra);
    move(IDC_RAW_LIST, 0, 0, horizontalExtra, mappingExtra);
    move(IDC_LPD_QUEUE_LIST, 0, 0, horizontalExtra, mappingExtra);
    move(IDC_COMMAND_GROUP, 0, mappingExtra, horizontalExtra, previewExtra);
    move(IDC_COMMAND_PREVIEW, 0, mappingExtra, horizontalExtra, previewExtra);
    for (const auto identifier : { IDC_INSTALL_UPDATE, IDC_START_SERVICE, IDC_STOP_SERVICE, IDC_RESTART_SERVICE,
        IDC_UNINSTALL_SERVICE, IDC_OPEN_LOGS, IDC_OPERATION_HINT }) {
        move(identifier, 0, verticalExtra);
    }
    move(IDCANCEL, horizontalExtra, verticalExtra);
}

void CManagerDialog::StartAction(Action action) {
    CancelStartupLoad();
    CString servicePath;
    m_servicePath.GetWindowTextW(servicePath);
    const auto attributes = servicePath.IsEmpty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(servicePath);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        AfxMessageBox(L"请选择存在的 PrintHub.exe 服务程序。", MB_ICONWARNING);
        return;
    }
    WorkItem work{ action, std::wstring(servicePath.GetString()), {}, QueryService(), GetSafeHwnd(),
        std::make_shared<OperationState>() };
    if (work.snapshot.error != ERROR_SUCCESS) {
        AfxMessageBox((CString(L"无法读取服务状态：") + FormatWin32Error(work.snapshot.error)), MB_ICONERROR);
        return;
    }
    if (action == Action::InstallOrUpdate) {
        const auto configuration = ReadConfiguration(true);
        if (!configuration) return;
        const auto validatePrinters = [](const auto& entries, const wchar_t* unavailablePrefix) {
            for (const auto& entry : entries) {
                const auto printer = ToWide(entry.printerName);
                if (!IsPrinterAvailable(printer)) {
                    AfxMessageBox((CString(unavailablePrefix) + printer.c_str()), MB_ICONWARNING);
                    return false;
                }
            }
            return true;
        };
        if (!validatePrinters(configuration->rawServers, L"RAW 打印机不可用：")
            || !validatePrinters(configuration->lpdQueues, L"LPD 映射打印机不可用：")) return;
        if (m_unknownExistingConfiguration && AfxMessageBox(L"现有服务配置包含无法识别的参数。继续将完全覆盖它，是否继续？", MB_ICONWARNING | MB_YESNO) != IDYES) return;
        if (work.snapshot.installed && AfxMessageBox(L"更新配置会停止、卸载、重新安装并启动服务。是否继续？", MB_ICONWARNING | MB_YESNO) != IDYES) return;
        work.configuration = *configuration;
    } else if (!work.snapshot.installed) {
        AfxMessageBox(L"服务尚未安装。", MB_ICONINFORMATION);
        return;
    }
    auto* parameter = new WorkItem(std::move(work));
    m_operationState = parameter->state;
    SetBusy(true);
    if (!AfxBeginThread(RunOperationThread, parameter)) {
        delete parameter;
        m_operationState.reset();
        SetBusy(false);
        AfxMessageBox(L"无法创建后台操作线程。", MB_ICONERROR);
    }
}

void CManagerDialog::SetBusy(bool busy) {
    m_busy = busy;
    for (const auto identifier : { IDC_BROWSE_SERVICE, IDC_REFRESH, IDC_ADD_RAW, IDC_REMOVE_RAW, IDC_ADD_LPD_QUEUE,
        IDC_REMOVE_LPD_QUEUE, IDC_INSTALL_UPDATE,
        IDC_START_SERVICE, IDC_STOP_SERVICE, IDC_RESTART_SERVICE, IDC_UNINSTALL_SERVICE, IDC_OPEN_LOGS }) {
        GetDlgItem(identifier)->EnableWindow(!busy);
    }
    m_servicePath.EnableWindow(!busy);
    m_listenAddress.EnableWindow(!busy);
    m_rawThreads.EnableWindow(!busy);
    m_printerName.EnableWindow(!busy);
    m_rawPort.EnableWindow(!busy);
    m_rawMappingPage.EnableWindow(!busy);
    m_lpdMappingPage.EnableWindow(!busy);
    m_lpdQueueName.EnableWindow(!busy);
    m_lpdPrinterName.EnableWindow(!busy);
    m_lpdPort.EnableWindow(!busy);
    m_lpdThreads.EnableWindow(!busy);
    m_maxPendingClients.EnableWindow(!busy);
    m_lpdMaxControlBytes.EnableWindow(!busy);
    m_snmpReject.EnableWindow(!busy);
    m_logLevel.EnableWindow(!busy);
    if (busy) m_status.SetWindowTextW(L"服务状态：正在执行操作...");
}

UINT __cdecl CManagerDialog::RunOperationThread(LPVOID parameter) {
    std::unique_ptr<WorkItem> work(static_cast<WorkItem*>(parameter));
    auto* result = new WorkResult(RunAction(*work));
    std::lock_guard<std::mutex> lock(work->state->mutex);
    if (work->state->cancelled || !::PostMessageW(work->targetWindow, kOperationCompletedMessage, 0,
        reinterpret_cast<LPARAM>(result))) {
        delete result;
    }
    return 0;
}

UINT __cdecl CManagerDialog::RunStartupLoadThread(LPVOID parameter) {
    std::unique_ptr<StartupLoadWorkItem> work(static_cast<StartupLoadWorkItem*>(parameter));
    auto result = std::make_unique<StartupLoadResult>(LoadStartupData());
    const auto& state = work->state;
    if (state->cancelled.load()) return 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->cancelled.load()) return 0;
        state->result = std::move(result);
    }
    if (!::PostMessageW(state->targetWindow, kStartupLoadCompletedMessage, 0, 0)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->result.reset();
    }
    return 0;
}

LRESULT CManagerDialog::OnOperationCompleted(WPARAM, LPARAM parameter) {
    std::unique_ptr<WorkResult> result(reinterpret_cast<WorkResult*>(parameter));
    m_operationState.reset();
    SetBusy(false);
    RefreshServiceStatus(false);
    CString message = result->success ? L"操作已完成。" : L"操作失败。";
    if (!result->details.IsEmpty()) message += L"\r\n\r\n" + result->details;
    AfxMessageBox(message, result->success ? MB_ICONINFORMATION : MB_ICONERROR);
    return 0;
}

LRESULT CManagerDialog::OnStartupLoadCompleted(WPARAM, LPARAM) {
    if (!m_startupLoadState || m_startupLoadState->cancelled.load()) return 0;
    std::unique_ptr<StartupLoadResult> result;
    {
        std::lock_guard<std::mutex> lock(m_startupLoadState->mutex);
        result = std::move(m_startupLoadState->result);
    }
    if (result) ApplyStartupLoadResult(*result);
    return 0;
}

CManagerDialog::WorkResult CManagerDialog::RunAction(const WorkItem& work) {
    const auto execute = [&work](ServiceManagerAction action) {
        std::vector<std::wstring> arguments;
        for (const auto& value : BuildServiceManagerArguments(work.configuration, action,
            ServiceConfigurationArgumentFormat::Explicit)) arguments.push_back(ToWide(value));
        return RunServiceCommand(work.executable, arguments);
    };
    if (work.action == Action::Start) return execute(ServiceManagerAction::Start);
    if (work.action == Action::Stop) return execute(ServiceManagerAction::Stop);
    if (work.action == Action::Restart) return execute(ServiceManagerAction::Restart);
    if (work.action == Action::Uninstall) return execute(ServiceManagerAction::Uninstall);

    for (const auto action : BuildConfigurationUpdateSequence(work.snapshot.installed, work.snapshot.running)) {
        auto result = execute(action);
        if (!result.success) return result;
        if (action == ServiceManagerAction::Uninstall && !WaitForServiceDeletion()) {
            return { false, L"服务已标记为删除，但未能在 10 秒内完成删除。请关闭其他服务管理工具后重试。" };
        }
    }
    return { true, L"" };
}

CManagerDialog::WorkResult CManagerDialog::RunServiceCommand(const std::wstring& executable, const std::vector<std::wstring>& arguments) {
    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    if (!CreatePipe(&outputRead, &outputWrite, &security, 0) || !SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0)) {
        const auto error = GetLastError();
        if (outputRead) CloseHandle(outputRead);
        if (outputWrite) CloseHandle(outputWrite);
        return { false, CString(L"无法创建输出管道：") + FormatWin32Error(error) };
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = outputWrite;
    startup.hStdError = outputWrite;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::vector<std::wstring> allArguments{ executable };
    allArguments.insert(allArguments.end(), arguments.begin(), arguments.end());
    auto commandLine = BuildWindowsCommandLine(allArguments);
    const BOOL created = CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, std::filesystem::path(executable).parent_path().c_str(), &startup, &process);
    CloseHandle(outputWrite);
    if (!created) {
        const auto error = GetLastError();
        CloseHandle(outputRead);
        return { false, CString(L"无法启动服务程序：") + FormatWin32Error(error) };
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = EXIT_FAILURE;
    GetExitCodeProcess(process.hProcess, &exitCode);
    std::string output;
    char buffer[512];
    DWORD read = 0;
    while (ReadFile(outputRead, buffer, sizeof(buffer), &read, nullptr) && read != 0) output.append(buffer, read);
    CloseHandle(outputRead);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CString details = ToWide(output).c_str();
    if (exitCode != EXIT_SUCCESS) {
        if (details.IsEmpty()) details.Format(L"服务程序退出代码：%lu", exitCode);
        return { false, details };
    }
    return { true, details };
}

CManagerDialog::ServiceSnapshot CManagerDialog::QueryService() {
    ServiceSnapshot snapshot;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) { snapshot.error = GetLastError(); return snapshot; }
    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!service) {
        const auto error = GetLastError();
        CloseServiceHandle(manager);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) return snapshot;
        snapshot.error = error;
        return snapshot;
    }
    snapshot.installed = true;
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) {
        snapshot.error = GetLastError();
    } else {
        snapshot.running = status.dwCurrentState == SERVICE_RUNNING;
    }
    DWORD configBytes = 0;
    QueryServiceConfigW(service, nullptr, 0, &configBytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || configBytes == 0) {
        snapshot.error = GetLastError();
    } else {
        std::vector<BYTE> buffer(configBytes);
        auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (QueryServiceConfigW(service, config, configBytes, &configBytes)) snapshot.imagePath = config->lpBinaryPathName;
        else snapshot.error = GetLastError();
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return snapshot;
}

bool CManagerDialog::WaitForServiceDeletion() {
    for (int attempt = 0; attempt < 240; ++attempt) {
        SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager) return false;
        SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
        const DWORD error = service ? ERROR_SUCCESS : GetLastError();
        if (service) CloseServiceHandle(service);
        CloseServiceHandle(manager);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        Sleep(250);
    }
    return false;
}

std::wstring CManagerDialog::DefaultServicePath() {
    wchar_t path[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return ((length == 0 ? std::filesystem::current_path() : std::filesystem::path(path).parent_path()) / L"PrintHub.exe").wstring();
}

std::wstring CManagerDialog::ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("无法将系统代码页文本转换为 Unicode");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string CManagerDialog::ToNarrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("文本无法转换为服务使用的系统代码页");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

CString CManagerDialog::FormatWin32Error(DWORD error) {
    LPWSTR message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    CString result;
    if (length != 0 && message) {
        result = message;
        result.Trim();
        LocalFree(message);
    } else {
        result.Format(L"错误代码 %lu", error);
    }
    return result;
}
