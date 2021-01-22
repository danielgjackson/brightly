// Monitor brightness control in the taskbar notification area
// Dan Jackson, 2020-2021.

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <tchar.h>

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>

#include <rpc.h>
#include <dbt.h>

#include "monitor.h"

// commctrl v6 for LoadIconMetric()
#include <commctrl.h>

// Notify icons
#include <shellapi.h>

// MSC-Specific Pragmas
#ifdef _MSC_VER
// Moved to external .manifest file linked through .rc file
//#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib") 	// InitCommonControlsEx(), LoadIconMetric()
#pragma comment(lib, "shell32.lib")  	// Shell_NotifyIcon()
#pragma comment(lib, "advapi32.lib") 	// RegOpenKeyEx(), RegSetValueEx(), RegGetValue(), RegDeleteValue(), RegCloseKey()
#pragma comment(lib, "Comdlg32.lib") 	// GetSaveFileName()
#pragma comment(lib, "Version.lib")		// GetFileVersionInfoSize(), GetFileVersionInfo(), VerQueryValue()
#pragma comment(lib, "gdi32.lib")		// CreateFontIndirect()
#pragma comment(lib, "User32.lib")		// Windows
#endif

// Missing define in gcc?
#ifndef _MSC_VER
extern int _dup2(int fd1, int fd2);
#endif

// Defines
#define TITLE TEXT("Brightly")
#define WMAPP_NOTIFYCALLBACK (WM_APP + 1)
#define IDM_OPEN		101
#define IDM_REFRESH		102
#define IDM_DEBUG		103
#define IDM_AUTOSTART	104
#define IDM_ABOUT		105
#define IDM_EXIT		106

// Hacky global state
HINSTANCE ghInstance = NULL;
HWND ghWndMain = NULL;
UINT idMinimizeIcon = 1;
BOOL gbHasNotifyIcon = FALSE;
BOOL gbAutoStart = FALSE;
BOOL gbNotify = TRUE;
BOOL gbPortable = FALSE;
BOOL gbAllowDuplicate = FALSE;
BOOL gbStartMinimized = TRUE;
BOOL gbSubsystemWindows = FALSE;
BOOL gbHasConsole = FALSE;
BOOL gbImmediatelyExit = FALSE;	// Quit right away (mainly for testing)
BOOL gbExiting = FALSE;
int gVersion[4] = { 0, 0, 0, 0 };

NOTIFYICONDATA nid = {0};

monitor_t *monitorList = NULL;

// Redirect standard I/O to a console
static BOOL RedirectIOToConsole(BOOL tryAttach, BOOL createIfRequired)
{
	BOOL hasConsole = FALSE;
	if (tryAttach && AttachConsole(ATTACH_PARENT_PROCESS))
	{
		hasConsole = TRUE;
	}
	if (createIfRequired && !hasConsole)
	{
		AllocConsole();
		AttachConsole(GetCurrentProcessId());
		hasConsole = TRUE;
	}
	if (hasConsole)
	{
		freopen("CONIN$", "r", stdin);
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);
	}
	return hasConsole;
}

void DeleteNotificationIcon(void)
{
	if (gbHasNotifyIcon)
	{
		nid.uFlags = 0;
		memset(&nid.guidItem, 0, sizeof(nid.guidItem));
		nid.uID = idMinimizeIcon;
		Shell_NotifyIcon(NIM_DELETE, &nid);
		gbHasNotifyIcon = FALSE;
	}
}

BOOL AddNotificationIcon(HWND hwnd)
{
	DeleteNotificationIcon();
	
	memset(&nid, 0, sizeof(nid));
	nid.cbSize = sizeof(nid);
	nid.hWnd = hwnd;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
	nid.uFlags |= NIS_HIDDEN;
	nid.dwStateMask = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
	if (gbNotify)
	{
		nid.uFlags |= NIF_INFO | NIF_REALTIME;
		_tcscpy(nid.szInfoTitle, TITLE);
		_tcscpy(nid.szInfo, TEXT("Running in notification area."));
		nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
		nid.hBalloonIcon = LoadIcon(ghInstance, TEXT("MAINICON"));
		nid.dwInfoFlags |= NIIF_LARGE_ICON | NIIF_USER;
	}
	memset(&nid.guidItem, 0, sizeof(nid.guidItem));
	nid.uID = idMinimizeIcon;
	nid.uCallbackMessage = WMAPP_NOTIFYCALLBACK;
	nid.hIcon = NULL;
	LoadIconMetric(ghInstance, L"MAINICON", LIM_SMALL, &nid.hIcon);	// MAKEINTRESOURCE(IDI_APPLICATION)
	_tcscpy(nid.szTip, TITLE);
	Shell_NotifyIcon(NIM_ADD, &nid);
	nid.uVersion = NOTIFYICON_VERSION_4;
	Shell_NotifyIcon(NIM_SETVERSION, &nid);
	gbHasNotifyIcon = TRUE;
	return gbHasNotifyIcon;
}


bool AutoStart(bool change, bool startup)
{
	bool retVal = false;
	HKEY hKeyMain = HKEY_CURRENT_USER;
	TCHAR *subKey = TEXT("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run");
	TCHAR *value = TITLE;

	TCHAR szModuleFileName[MAX_PATH];
	GetModuleFileName(NULL, szModuleFileName, sizeof(szModuleFileName) / sizeof(szModuleFileName[0]));

	TCHAR szFileName[MAX_PATH + 32];
	_sntprintf(szFileName, sizeof(szFileName) / sizeof(szFileName[0]), TEXT("\"%Ts\" /AUTOSTART"), szModuleFileName);

	if (change)
	{
		// Set the key
		HKEY hKey = NULL;
		LSTATUS lErrorCode = RegOpenKeyEx(hKeyMain, subKey, 0, KEY_SET_VALUE, &hKey);
		if (lErrorCode == ERROR_SUCCESS)
		{
			if (startup)
			{
				// Setting to auto-start
				lErrorCode = RegSetValueEx(hKey, value, 0, REG_SZ, (const BYTE *)szFileName, (_tcslen(szFileName) + 1) * sizeof(TCHAR));
				if (lErrorCode == ERROR_SUCCESS)
				{
					retVal = true;
				}
			}
			else
			{
				// Setting to not auto-start
				lErrorCode = RegDeleteValue(hKey, value);
				if (lErrorCode == ERROR_SUCCESS)
				{
					retVal = true;
				}
			}
			RegCloseKey(hKey);
		}
	}
	else
	{
		// Query that the key is set and has the correct value
		TCHAR szData[MAX_PATH];
		DWORD cbData = sizeof(szData);
		LSTATUS lErrorCode = RegGetValue(hKeyMain, subKey, value, RRF_RT_REG_SZ, NULL, &szData, &cbData);
		if (lErrorCode == ERROR_SUCCESS && _tcscmp(szData, szFileName) == 0) {
			retVal = true;
		}
	}
	return retVal;
}


void ShowContextMenu(HWND hwnd, POINT pt)
{
	UINT autoStartFlags;
	// Don't touch the registry if running as a portable app
	if (!gbPortable) {
		autoStartFlags = AutoStart(false, false) ? MF_CHECKED : MF_UNCHECKED;
	} else {
		autoStartFlags = MF_UNCHECKED | MF_GRAYED;
	}

	HMENU hMenu = CreatePopupMenu();
	AppendMenu(hMenu, MF_STRING, IDM_OPEN, TEXT("&Open"));
	AppendMenu(hMenu, MF_STRING, IDM_REFRESH, TEXT("&Refresh"));
	AppendMenu(hMenu, MF_STRING, IDM_DEBUG, TEXT("Save &Debug Info..."));
	AppendMenu(hMenu, MF_STRING | autoStartFlags, IDM_AUTOSTART, TEXT("Auto-&Start"));
	AppendMenu(hMenu, MF_STRING, IDM_ABOUT, TEXT("&About"));
	AppendMenu(hMenu, MF_SEPARATOR, 0, TEXT("Separator"));
	AppendMenu(hMenu, MF_STRING, IDM_EXIT, TEXT("E&xit"));
	SetMenuDefaultItem(hMenu, IDM_OPEN, FALSE);
	SetForegroundWindow(hwnd);
	UINT uFlags = TPM_RIGHTBUTTON;
	if (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0)
	{
		uFlags |= TPM_RIGHTALIGN;
	}
	else
	{
		uFlags |= TPM_LEFTALIGN;
	}
	TrackPopupMenuEx(hMenu, uFlags, pt.x, pt.y, hwnd, NULL);
	DestroyMenu(hMenu);
}

void DumpMonitors(FILE *file, bool details)
{
	for (monitor_t *monitor = monitorList; monitor != NULL; monitor = monitor->next)
	{
		bool hasBrightness = MonitorHasBrightness(monitor);
		int brightness = MonitorGetBrightness(monitor);
		const wchar_t *description = MonitorGetDescription(monitor);
		
		_ftprintf(file, TEXT("*** MONITOR #%d: %ls [hasBrightness=%d] @%d%%\n"), monitor->index, description, hasBrightness ? 1 : 0, brightness);
		if (details) MonitorDump(file, monitor);

		// When debugging, just toggle between 50% and 100% on all monitor.
		if (gbImmediatelyExit)
		{
			if (hasBrightness)
			{
				int value = (brightness == 50) ? 100 : 50;
				_ftprintf(file, TEXT(">>> setting to %d%%\n"), value);
				MonitorSetBrightness(monitor, value);
			}
		}
	}
}

void SearchMonitors(void)
{
	if (monitorList != NULL)
	{
		MonitorListDestroy(monitorList);
		monitorList = NULL;
	}
	monitorList = MonitorListEnumerate();

	DumpMonitors(stdout, false);
}

BOOL HasExistingInstance(void)
{
    HANDLE hStartEvent = CreateEvent(NULL, FALSE, FALSE, TEXT("Local\\brightly"));	// Session namespace is ok (does not need to be Global)
	DWORD lastError = GetLastError();
	if (hStartEvent && !lastError)
	{
		// We are the first instance (hold on to the event, it will only be released when the process ends)
		_ftprintf(stderr, TEXT("INSTANCE: No other instance found.\n"));
		hStartEvent = NULL;
		return FALSE;
	}
    else if (hStartEvent && lastError == ERROR_ALREADY_EXISTS)
	{
		// There is another instance running
		_ftprintf(stderr, TEXT("INSTANCE: Another instance found.\n"));
		CloseHandle(hStartEvent);
		hStartEvent = NULL;
		return TRUE;
	}
	else
	{
		// Otherwise, fail safe and assume not already running
		_ftprintf(stderr, TEXT("INSTANCE: Problem finding instance.\n"));
	    return FALSE;
	}
}

HFONT hDlgFont = NULL;
int numMonitors = 0;		// Number of controls when window was created
bool windowOpen = false;
#define ID_LABEL_BASE 3000
#define ID_LABEL_END (ID_LABEL_BASE + 999)
#define ID_TRACKBAR_BASE 4000
#define ID_TRACKBAR_END (ID_TRACKBAR_BASE + 999)

void RemoveControls(void)
{
	// Remove components
	for (int i = 0; i < numMonitors; i++)
	{
		DestroyWindow(GetDlgItem(ghWndMain, ID_LABEL_BASE + i));
		DestroyWindow(GetDlgItem(ghWndMain, ID_TRACKBAR_BASE + i));
	}
	numMonitors = 0;
}

void CreateControls(void)
{
	int xMargin = 10;
	int width = 200;
	int yMargin = 10;
	int yStep = 60;
	
	RemoveControls();

	HWND hWnd = ghWndMain;
	HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE); // ghInstance;

	// Populate  // GetDlgItem(hWnd, id)
	for (monitor_t *monitor = monitorList; monitor != NULL; monitor = monitor->next)
	{
		int y = yMargin + numMonitors * yStep;

		// Create components
		HWND hWndLabel = CreateWindowEx(0, TEXT("STATIC"), MonitorGetDescription(monitor), WS_VISIBLE | WS_CHILD | SS_ENDELLIPSIS, 10, y, width, 20, hWnd, (HMENU)(intptr_t)(ID_LABEL_BASE + numMonitors), hInstance, NULL);
		SendMessage(hWndLabel, WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(FALSE, 0));

		HWND hWndTrack = CreateWindowEx(0, TRACKBAR_CLASS, TEXT("Trackbar Control"), WS_CHILD | WS_VISIBLE | TBS_HORZ | WS_TABSTOP | TBS_AUTOTICKS | TBS_DOWNISLEFT, 10, y + 20, width, 30, hWnd, (HMENU)(intptr_t)(ID_TRACKBAR_BASE + numMonitors), hInstance, NULL); 
		SendMessage(hWndTrack, TBM_SETRANGE, (WPARAM)TRUE, (LPARAM)MAKELONG(0, 100));
		SendMessage(hWndTrack, TBM_SETTICFREQ , (WPARAM)10, (LPARAM)0);
		SendMessage(hWndTrack, TBM_SETPAGESIZE, 0, (LPARAM)10);
		SendMessage(hWndTrack, TBM_SETPOS, (WPARAM)TRUE, (LPARAM)MonitorGetBrightness(monitor));

		if (!MonitorHasBrightness(monitor))
		{
			EnableWindow(hWndLabel, FALSE);
			EnableWindow(hWndTrack, FALSE);
		}

		numMonitors++;
	}

	// Size
	SIZE windowSize;
	windowSize.cx = width + 2 * xMargin;
	windowSize.cy = numMonitors * yStep + 2 * yMargin;
	SetWindowPos(hWnd, NULL, 0, 0, windowSize.cx, windowSize.cy, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
}

void PositionWindow(int x, int y)
{
	RECT rect = {0};
	GetWindowRect(ghWndMain, &rect);
	SIZE windowSize;
	windowSize.cx = rect.right - rect.left;
	windowSize.cy = rect.bottom - rect.top;

	// Fallback to (0,0)
	RECT rectSpace = {0};

	// Prefer desktop window
	GetWindowRect(GetDesktopWindow(), &rectSpace);

	// Prefer the work area
	SystemParametersInfo(SPI_GETWORKAREA, 0, &rectSpace, 0);

	// Default to the lower-right corner
	rect.left = 0;
	rect.top = 0;
	if (rectSpace.right - rectSpace.left > 0) rect.left = rectSpace.right - windowSize.cx;
	if (rectSpace.bottom - rectSpace.top > 0) rect.top = rectSpace.bottom - windowSize.cy;
	rect.top = rectSpace.bottom - windowSize.cy;
	rect.right = rect.left + windowSize.cx;
	rect.bottom = rect.top + windowSize.cy;

	// Prefer a calculated position
	POINT anchorPoint;
	anchorPoint.x = x;
	anchorPoint.y = y;
	CalculatePopupWindowPosition(&anchorPoint, &windowSize, TPM_CENTERALIGN | TPM_BOTTOMALIGN | TPM_VERTICAL | TPM_WORKAREA, NULL, &rect);

	AdjustWindowRect(&rect, GetWindowLong(ghWndMain, GWL_STYLE), FALSE);
	SetWindowPos(ghWndMain, 0, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void OpenWindow(int x, int y)
{
	MonitorListRefreshBrightness(monitorList);
	CreateControls();
	PositionWindow(x, y);
	ShowWindow(ghWndMain, SW_SHOW);
	SetForegroundWindow(ghWndMain);
	windowOpen = true;
}

void HideWindow(void)
{
	ShowWindow(ghWndMain, SW_HIDE);
	RemoveControls();
	windowOpen = false;
}

void DevicesChanged(void)
{
	SearchMonitors();
	if (windowOpen)
	{
		RemoveControls();
		CreateControls();
	}
}

void StartExit(void)
{
	gbExiting = TRUE;
	PostMessage(ghWndMain, WM_CLOSE, 0, 0);
}

void Startup(HWND hWnd)
{
	_tprintf(TEXT("Startup...\n"));

	ghWndMain = hWnd;

	BOOL duplicateInstance = FALSE;
	int response = 0;
	do
	{
		if (gbAllowDuplicate) break;
		duplicateInstance = HasExistingInstance();
		if (!duplicateInstance) break;
	    response = MessageBox(NULL, TEXT("Brightly is already running in the notification area.\r\n\r\nIf you continue, you will allow a duplicate instance to run."), TITLE, MB_ICONWARNING | MB_CANCELTRYCONTINUE | MB_DEFBUTTON1);
		if (response == IDCANCEL)
		{
			gbImmediatelyExit = TRUE;
		}
	} while (response == IDTRYAGAIN);

	// Only continue normal start-up if not cancelling
	if (response != IDCANCEL)
	{
		DevicesChanged();
		AddNotificationIcon(ghWndMain);
	}

	if (gbImmediatelyExit) StartExit();
}

void Shutdown(void)
{
	_tprintf(TEXT("Shutdown()...\n"));
	DeleteNotificationIcon();
	_tprintf(TEXT("...END: Shutdown()\n"));
}

// Open hyperlink from TaskDialog
HRESULT CALLBACK TaskDialogCallback(HWND hwnd, UINT uNotification, WPARAM wParam, LPARAM lParam, LONG_PTR dwRefData)
{
	if (uNotification == TDN_HYPERLINK_CLICKED)
	{
		ShellExecute(hwnd, TEXT("open"), (TCHAR *)lParam, NULL, NULL, SW_SHOW);
	}
	return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_CREATE:
		Startup(hwnd);
		break;

	case WM_DISPLAYCHANGE:
		{
			_tprintf(TEXT("WM_DISPLAYCHANGE\n"));
			DevicesChanged();
			_tprintf(TEXT("/WM_DISPLAYCHANGE\n"));
		}
		break;

	case WM_DEVICECHANGE:
		{
			if (
				//wParam == DBT_CONFIGCHANGED || 
				//wParam == DBT_DEVICEARRIVAL || 
				wParam == DBT_DEVNODES_CHANGED)
			{
				_tprintf(TEXT("WM_DEVICECHANGE:%s\n"), 
					//wParam == DBT_CONFIGCHANGED ? TEXT("DBT_CONFIGCHANGED") :
					//wParam == DBT_DEVICEARRIVAL ? TEXT("DBT_DEVICEARRIVAL") :
					wParam == DBT_DEVNODES_CHANGED ? TEXT("wParam == DBT_DEVNODES_CHANGED") :
					TEXT("?")
				);
				DevicesChanged();
				_tprintf(TEXT("/WM_DEVICECHANGE\n"));
			}
		}
		break;

	case WM_ACTIVATE:
		{
			if (wParam == WA_INACTIVE)
			{
				HideWindow();
			}
		}
		break;

	// Handled pre-translate message
	// case WM_KEYDOWN:
	// 	if (wParam == VK_ESCAPE)
	// 	{
	// 		HideWindow();
	// 	}
	// 	break;		

	case WM_CTLCOLORSTATIC:
	{
		//HDC hdcStatic = (HDC)wParam;
        //SetTextColor(hdcStatic, RGB(255,255,255));
        //SetBkColor(hdcStatic, RGB(0,0,0));
        return (INT_PTR)GetSysColorBrush(COLOR_WINDOW); // CreateSolidBrush(RGB(0,0,0));
	}
	break;

	case WM_COMMAND:
		{
			int const wmId = LOWORD(wParam);
			switch (wmId)
			{
			case IDM_OPEN:
				{
					POINT mouse;
					GetCursorPos(&mouse);
					OpenWindow(mouse.x, mouse.y);
				}
				break;
			case IDM_REFRESH:
				{
					DevicesChanged();
				}
				break;
			case IDM_DEBUG:
				{
					// Initial filename from local date/time
					TCHAR szFileName[MAX_PATH] = TEXT("");
					time_t now;
					time(&now);
					struct tm *local = localtime(&now);
					_sntprintf(szFileName, sizeof(szFileName) / sizeof(szFileName[0]), TEXT("" TITLE "_%04d-%02d-%02d_%02d-%02d-%02d.txt"), local->tm_year + 1900, local->tm_mon + 1, local->tm_mday, local->tm_hour, local->tm_min, local->tm_sec);

					OPENFILENAME openFilename = {0};
					openFilename.lStructSize = sizeof(openFilename);
					openFilename.hwndOwner = hwnd;
					openFilename.lpstrFilter = TEXT("Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0");
					openFilename.lpstrFile = szFileName;
					openFilename.nMaxFile = sizeof(szFileName) / sizeof(*szFileName);
					openFilename.Flags = OFN_SHOWHELP | OFN_OVERWRITEPROMPT;
					openFilename.lpstrDefExt = (LPCWSTR)L"txt";
					openFilename.lpstrTitle = TITLE;

					// Write file to temp folder
					TCHAR szInitialDir[MAX_PATH] = TEXT("");
					GetTempPath(sizeof(szInitialDir), szInitialDir);
					openFilename.lpstrInitialDir = szInitialDir[0] ? szInitialDir : NULL;

					if (GetSaveFileName(&openFilename))
					{
						FILE *file = _tfopen(openFilename.lpstrFile, TEXT("w"));
						if (file)
						{
							DumpMonitors(file, true);
							fclose(file);
							ShellExecute(NULL, TEXT("open"), openFilename.lpstrFile, NULL, NULL, SW_SHOW);
						}
					}
				}
				break;
			case IDM_AUTOSTART:
				if (AutoStart(false, false))
				{
					// Is auto-starting, remove
					AutoStart(true, false);
				}
				else
				{
					// Is not auto-starting, add
					AutoStart(true, true);
				}
				break;
			case IDM_ABOUT:
				{
					TCHAR *szTitle = TEXT("About " TITLE);
					TCHAR szHeader[128] = TEXT("");
					_sntprintf(szHeader, sizeof(szHeader) / sizeof(szHeader[0]), TEXT("%s V%d.%d.%d"), TITLE, gVersion[0], gVersion[1], gVersion[2]);
					TCHAR *szContent = TEXT("Monitor brightness adjustment in the taskbar notification area.");
					//TCHAR *szExtraInfo = TEXT("...");
					TCHAR *szFooter = TEXT("Open source under <a href=\"https://github.com/danielgjackson/brightly/blob/master/LICENSE.txt\">MIT License</a>, \u00A92020 Daniel Jackson.");
					TASKDIALOG_BUTTON aCustomButtons[] = {
						{ 1001, L"Project page\ngithub.com/danielgjackson/brightly" },
						{ 1002, L"Check for updates\nSee the latest release" },
					};
					TASKDIALOGCONFIG tdc = {0};
					tdc.cbSize = sizeof(tdc);
					tdc.hwndParent = ghWndMain;
					tdc.dwFlags = TDF_USE_HICON_MAIN | TDF_USE_COMMAND_LINKS | TDF_ENABLE_HYPERLINKS | TDF_EXPANDED_BY_DEFAULT | TDF_EXPAND_FOOTER_AREA | TDF_ALLOW_DIALOG_CANCELLATION;
					tdc.pButtons = aCustomButtons;
					tdc.cButtons = sizeof(aCustomButtons) / sizeof(aCustomButtons[0]);
					tdc.pszWindowTitle = szTitle;
					tdc.nDefaultButton = IDOK;
					tdc.hMainIcon = LoadIcon(ghInstance, TEXT("MAINICON"));
					tdc.pszMainInstruction = szHeader;
					tdc.pszContent = szContent;
					//tdc.pszExpandedInformation = szExtraInfo;
					tdc.pszFooter = szFooter;
					tdc.pszFooterIcon = TD_INFORMATION_ICON;
					tdc.dwCommonButtons = TDCBF_OK_BUTTON;
					tdc.pfCallback = TaskDialogCallback;
					tdc.lpCallbackData = (LONG_PTR)NULL;	// no context needed
					int nClickedBtn;
					HRESULT hr = TaskDialogIndirect(&tdc, &nClickedBtn, NULL, NULL);	// nClickedBtn == IDOK
					if (SUCCEEDED(hr))
					{
						if (nClickedBtn == 1001)
						{
							ShellExecute(hwnd, TEXT("open"), TEXT("https://github.com/danielgjackson/brightly/"), NULL, NULL, SW_SHOW);
						}
						else if (nClickedBtn == 1002)
						{
							ShellExecute(hwnd, TEXT("open"), TEXT("https://github.com/danielgjackson/brightly/releases/latest"), NULL, NULL, SW_SHOW);
						}
					}
				}
				break;
			case IDM_EXIT:
				StartExit();
				break;
			default:
				return DefWindowProc(hwnd, message, wParam, lParam);
			}
		}
		break;

	case WMAPP_NOTIFYCALLBACK:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
			{
				POINT mouse;
				mouse.x = LOWORD(wParam);
				mouse.y = HIWORD(wParam);
				OpenWindow(mouse.x, mouse.y);
			}
			break;

		case WM_RBUTTONUP:		// If not using NOTIFYICON_VERSION_4 ?
		case WM_CONTEXTMENU:
			{
				POINT const pt = { LOWORD(wParam), HIWORD(wParam) };
				ShowContextMenu(hwnd, pt);
			}
			break;
		}
		break;

	case WM_ENDSESSION:
		StartExit();
		break;

	case WM_CLOSE:
		if (gbExiting)
		{
			DestroyWindow(hwnd);
		}
		else
		{
			ShowWindow(hwnd, SW_HIDE);
		}
		break;

	case WM_DESTROY:
		Shutdown();
		PostQuitMessage(0);
		break;

	case WM_HSCROLL:
		{
			HWND hWndControl = (HWND)lParam;
			int id = GetWindowLong(hWndControl, GWL_ID);
			if (id >= ID_TRACKBAR_BASE && id <= ID_TRACKBAR_END)
			{
				int index = id - ID_TRACKBAR_BASE;
				int value;
				// if (LOWORD(wParam) == TB_THUMBPOSITION || LOWORD(wParam) == TB_THUMBTRACK) value = HIWORD(wParam);
				value = SendMessage(hWndControl, TBM_GETPOS, 0, 0);
				//_tprintf(TEXT("#%d @%d\n"), index, value);
				int i = 0;
				for (monitor_t *monitor = monitorList; monitor != NULL; monitor = monitor->next)
				{
					if (i == index)
					{
						MonitorSetBrightness(monitor, value);
						break;
					}
					i++;
				}

			}
		}	

	default:
		return DefWindowProc(hwnd, message, wParam, lParam);
	}
	return 0;
}

void done(EXCEPTION_POINTERS *exceptionInfo)
{
	if (exceptionInfo)
	{
		TCHAR msg[512] = TEXT("");
		_sntprintf(msg, sizeof(msg) / sizeof(msg[0]), TEXT("ERROR: An unhandled error has occurred."), TITLE);
		// [/CONSOLE:<ATTACH|CREATE|ATTACH-CREATE>]*  (* only as first parameter)
		if (gbHasConsole)
		{
			_tprintf(msg);
		}
		MessageBox(NULL, msg, TITLE, MB_OK | MB_ICONERROR);
	}
	_tprintf(TEXT("DONE.\n"));
}

void finish(void)
{
	_tprintf(TEXT("END: Application ended.\n"));
	done(NULL);
}

LONG WINAPI UnhandledException(EXCEPTION_POINTERS *exceptionInfo)
{
	_tprintf(TEXT("END: Unhandled exception.\n"));
	done(exceptionInfo);
	return EXCEPTION_EXECUTE_HANDLER; // EXCEPTION_EXECUTE_HANDLER; // EXCEPTION_CONTINUE_SEARCH;
}

void SignalHandler(int signal)
{
	_tprintf(TEXT("END: Received signal: %d\n"), signal);
	//if (signal == SIGABRT)
	done(NULL);
}

BOOL WINAPI consoleHandler(DWORD signal)
{
	_tprintf(TEXT("END: Received console control event: %u\n"), (unsigned int)signal);
	//if (signal == CTRL_C_EVENT)
	done(NULL);
	return FALSE;
}

int run(int argc, TCHAR *argv[], HINSTANCE hInstance, BOOL hasConsole)
{
	_ftprintf(stderr, TEXT("run()\n"));

	ghInstance = hInstance;
	gbHasConsole = hasConsole;

	if (gbHasConsole)
	{
		SetConsoleCtrlHandler(consoleHandler, TRUE);
	}
	signal(SIGABRT, SignalHandler);
	SetUnhandledExceptionFilter(UnhandledException);
	atexit(finish);

	ghInstance = hInstance;

	// Get module filename
	TCHAR szModuleFileName[MAX_PATH] = {0};
	GetModuleFileName(NULL, szModuleFileName, sizeof(szModuleFileName) / sizeof(szModuleFileName[0]));

	// Detect if the executable is named as a "PortableApps"-compatible ".paf.exe" -- ensure no registry changes, no check for updates, etc.
	if (_tcslen(szModuleFileName) > 8)
	{
		if (_tcsicmp(szModuleFileName + _tcslen(szModuleFileName) - 8, TEXT(".paf.exe")) == 0)
		{
			gbPortable = TRUE;
		}
	}

	// Get module version
	DWORD dwVerHandle = 0;
	DWORD dwVerLen = GetFileVersionInfoSize(szModuleFileName, &dwVerHandle);
	if (dwVerLen > 0)
	{
		LPVOID verData = malloc(dwVerLen);
		if (GetFileVersionInfo(szModuleFileName, dwVerHandle, dwVerLen, verData))
		{
			VS_FIXEDFILEINFO *pFileInfo = NULL;
			UINT uLenFileInfo = 0;
			if (VerQueryValue(verData, TEXT("\\"), (LPVOID*)&pFileInfo, &uLenFileInfo))
			{
				gVersion[0] = HIWORD(pFileInfo->dwProductVersionMS);	// Major
				gVersion[1] = LOWORD(pFileInfo->dwProductVersionMS);	// Minor
				gVersion[2] = HIWORD(pFileInfo->dwProductVersionLS);	// Patch ('build' in MS version order)
				gVersion[3] = LOWORD(pFileInfo->dwProductVersionLS);	// Build ('revision' in MS version order)
				//_ftprintf(stderr, TEXT("V%u.%u.%u.%u\n"), gVersion[0], gVersion[1], gVersion[2], gVersion[3]);
			}
		}
		free(verData);
	}

	BOOL bShowHelp = FALSE;
	int positional = 0;
	int errors = 0;

	for (int i = 0; i < argc; i++)  
	{
		if (_tcsicmp(argv[i], TEXT("/?")) == 0) { bShowHelp = TRUE; }
		else if (_tcsicmp(argv[i], TEXT("/HELP")) == 0) { bShowHelp = TRUE; }
		else if (_tcsicmp(argv[i], TEXT("--help")) == 0) { bShowHelp = TRUE; }
		else if (_tcsicmp(argv[i], TEXT("/MIN")) == 0) { gbStartMinimized = TRUE; }
		else if (_tcsicmp(argv[i], TEXT("/NOMIN")) == 0) { gbStartMinimized = FALSE; }
		else if (_tcsicmp(argv[i], TEXT("/AUTOSTART")) == 0) { gbAutoStart = TRUE; gbNotify = FALSE; }	// Don't notify when auto-starting
		else if (_tcsicmp(argv[i], TEXT("/NOTIFY")) == 0) { gbNotify = TRUE; }
		else if (_tcsicmp(argv[i], TEXT("/NONOTIFY")) == 0) { gbNotify = FALSE; }
		else if (_tcsicmp(argv[i], TEXT("/PORTABLE")) == 0) { gbPortable = TRUE; }
		else if (_tcsicmp(argv[i], TEXT("/NOPORTABLE")) == 0) { gbPortable = FALSE; }	// Allow override
		else if (_tcsicmp(argv[i], TEXT("/ALLOWDUPLICATE")) == 0) { gbAllowDuplicate = TRUE; }
		else if (_tcsicmp(argv[i], TEXT("/EXIT")) == 0) { gbImmediatelyExit = TRUE; }
		
		else if (argv[i][0] == '/') 
		{
			_ftprintf(stderr, TEXT("ERROR: Unexpected parameter: %s\n"), argv[i]);
			errors++;
		} 
		else 
		{
			/*
			if (positional == 0)
			{
				argv[i];
			} 
			else
			*/
			{
				_ftprintf(stderr, TEXT("ERROR: Unexpected positional parameter: %s\n"), argv[i]);
				errors++;
			}
			positional++;
		}
	}

	if (gbPortable)
	{
		_ftprintf(stdout, TEXT("NOTE: Running as a portable app.\n"));
	}

	if (errors)
	{
		_ftprintf(stderr, TEXT("ERROR: %d parameter error(s).\n"), errors);
		bShowHelp = true;
	}

	if (bShowHelp) 
	{
		TCHAR msg[512] = TEXT("");
		_sntprintf(msg, sizeof(msg) / sizeof(msg[0]), TEXT("%s V%d.%d.%d  Daniel Jackson, 2020.\n\nUsage: [/NOMIN|/MIN]\n\n"), TITLE, gVersion[0], gVersion[1], gVersion[2]);
		// [/CONSOLE:<ATTACH|CREATE|ATTACH-CREATE>]*  (* only as first parameter)
		if (gbHasConsole)
		{
			_tprintf(msg);
		}
		else
		{
			MessageBox(NULL, msg, TITLE, MB_OK | MB_ICONERROR);
		}
		return -1;
	}

	// Initialize COM
	HRESULT hr;
	hr = CoInitializeEx(0, COINIT_MULTITHREADED);
	if (FAILED(hr)) { fprintf(stderr, "ERROR: Failed CoInitializeEx().\n"); return 1; }
	hr = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
	if (FAILED(hr)) { fprintf(stderr, "ERROR: Failed CoInitializeSecurity().\n"); return 2; }

	// Initialize common controls
	INITCOMMONCONTROLSEX icce = {0};
	icce.dwSize = sizeof(icce);
	icce.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
	if (!InitCommonControlsEx(&icce))
	{
		fprintf(stderr, "WARNING: Failed InitCommonControlsEx().\n");
	}

	const TCHAR szWindowClass[] = TITLE;
	WNDCLASSEX wcex = {sizeof(wcex)};
	wcex.style			= CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc	= WndProc;
	wcex.hInstance		= ghInstance;
	wcex.hIcon			= LoadIcon(ghInstance, TEXT("MAINICON"));
	wcex.hCursor		= LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground	= (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName	= NULL;
	wcex.lpszClassName	= szWindowClass;
	RegisterClassEx(&wcex);

	TCHAR *szTitle = TITLE;
	DWORD dwStyle = 0; // WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_POPUP | WS_VISIBLE | WS_THICKFRAME
	DWORD dwStyleEx = WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST; // (WS_EX_TOOLWINDOW) & ~(WS_EX_APPWINDOW);

	HWND hWnd = CreateWindowEx(dwStyleEx, szWindowClass, szTitle, dwStyle, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, ghInstance, NULL);

	// Remove caption
	SetWindowLong(hWnd, GWL_STYLE, GetWindowLong(hWnd, GWL_STYLE) & ~(WS_CAPTION));

	// Fix font
	NONCLIENTMETRICS ncm;
	ncm.cbSize = sizeof(ncm);
	SystemParametersInfo(SPI_GETNONCLIENTMETRICS, ncm.cbSize, &ncm, 0);
	hDlgFont = CreateFontIndirect(&(ncm.lfMessageFont));
	SendMessage(hWnd, WM_SETFONT, (WPARAM)hDlgFont, MAKELPARAM(FALSE, 0));

	ghWndMain = hWnd;
	if (!hWnd) { return -1; }

	HideWindow();  // nCmdShow
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		// Pre-translate so captured before child controls
		if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
			HideWindow();
        }
		if (!IsDialogMessage(hWnd, &msg))	// handles tabbing etc (avoid WM_USER=DM_GETDEFID / WM_USER+1=DM_SETDEFID)
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	CoUninitialize();

	return 0;
}

// Entry point when compiled with console subsystem
int _tmain(int argc, TCHAR *argv[]) 
{
	HINSTANCE hInstance = (HINSTANCE)GetModuleHandle(NULL);
	return run(argc - 1, argv + 1, hInstance, TRUE);
}

// Entry point when compiled with Windows subsystem
int APIENTRY _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
	gbSubsystemWindows = TRUE;
	
	int argc = 0;
	TCHAR **argv = CommandLineToArgvW(GetCommandLine(), &argc);	// Must be UNICODE for CommandLineToArgvW()

	BOOL bConsoleAttach = FALSE;
	BOOL bConsoleCreate = FALSE;
	BOOL hasConsole = FALSE;
	int argOffset = 1;
	for (; argc > argOffset; argOffset++)
	{
		if (_tcsicmp(argv[argOffset], TEXT("/CONSOLE:ATTACH")) == 0)
		{
			bConsoleAttach = TRUE;
		}
		else if (_tcsicmp(argv[argOffset], TEXT("/CONSOLE:CREATE")) == 0)
		{
			bConsoleCreate = TRUE;
		}
		else if (_tcsicmp(argv[argOffset], TEXT("/CONSOLE:ATTACH-CREATE")) == 0)
		{
			bConsoleAttach = TRUE;
			bConsoleCreate = TRUE;
		}
		else if (_tcsicmp(argv[argOffset], TEXT("/CONSOLE:DEBUG")) == 0)	// Use existing console
		{
			_dup2(fileno(stderr), fileno(stdout));	// stdout to stderr (stops too much interleaving from buffering while debugging)
			fflush(stdout);
			hasConsole = TRUE;
		}
		else	// No more prefix arguments
		{
			//_ftprintf(stderr, TEXT("NOTE: Stopped finding prefix parameters at: %s\n"), argv[argOffset]);
			break;
		}
	}
	if (!hasConsole) {
		hasConsole = RedirectIOToConsole(bConsoleAttach, bConsoleCreate);
	}
	return run(argc - argOffset, argv + argOffset, hInstance, hasConsole);
}
