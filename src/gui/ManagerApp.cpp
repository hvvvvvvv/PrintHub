#include "ManagerDialog.h"

class PrintHubManagerApplication final : public CWinApp {
public:
    BOOL InitInstance() override {
        CWinApp::InitInstance();
        INITCOMMONCONTROLSEX commonControls{ sizeof(commonControls), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&commonControls);
        CManagerDialog dialog;
        m_pMainWnd = &dialog;
        dialog.DoModal();
        return FALSE;
    }
};

PrintHubManagerApplication theApplication;

// Static MFC supplies AfxWinMain in uafxcw.lib. Defining the Unicode entry
// point locally makes the CRT select it even when CMake uses the Ninja linker.
extern int AFXAPI AfxWinMain(HINSTANCE instance, HINSTANCE previousInstance,
                             LPWSTR commandLine, int showCommand);

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previousInstance, PWSTR commandLine, int showCommand) {
    return AfxWinMain(instance, previousInstance, commandLine, showCommand);
}
