#include "CWinService.h"

CWinService CWinService::This;
SERVICE_STATUS CWinService::serviceStatus;
SERVICE_STATUS_HANDLE CWinService::serviceStatusHandle;
HANDLE CWinService::hServerStopEvent;

CWinService::CWinService()
{
	//初期化
	hServerStopEvent = NULL;

	// サービス名を設定する
	::GetModuleFileName(NULL, serviceExePath, BUFSIZ);
	::_tsplitpath_s(serviceExePath, NULL, 0, NULL, 0, serviceName, BUFSIZ, NULL, 0);
}

CWinService::~CWinService()
{
}

//
// SCMへのインストール
//
BOOL CWinService::Install()
{
	SC_HANDLE hManager = NULL;
	SC_HANDLE hService = NULL;
	BOOL ret = FALSE;

	do
	{
		hManager = ::OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
		if (hManager == NULL)
			break;

		hService = ::CreateService(hManager,
			serviceName,				// service name
			serviceName,				// service name to display
			0,							// desired access
			SERVICE_WIN32_OWN_PROCESS,	// service type
			SERVICE_DEMAND_START,		// start type
			SERVICE_ERROR_NORMAL,		// error control type
			serviceExePath,				// service's binary
			NULL,						// no load ordering group
			NULL,						// no tag identifier
			NULL,						// no dependencies
			NULL,						// LocalSystem account
			NULL);						// no password
		if (hService == NULL)
			break;

		ret = TRUE;
	}
	while (0);

	if (hService)
		::CloseServiceHandle(hService);

	if (hManager)
		::CloseServiceHandle(hManager);

	return ret;
}

//
// SCMから削除
//
BOOL CWinService::Remove()
{
	SC_HANDLE hManager = NULL;
	SC_HANDLE hService = NULL;
	BOOL ret = FALSE;

	do
	{
		hManager = ::OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
		if (hManager == NULL)
			break;

		hService = ::OpenService(hManager, serviceName, DELETE);
		if (hService == NULL)
			break;

		if (::DeleteService(hService) == FALSE)
			break;

		ret = TRUE;
	}
	while (0);

	if (hService)
		::CloseServiceHandle(hService);

	if (hManager)
		::CloseServiceHandle(hManager);

	return ret;
}

//
// サービス起動
//
BOOL CWinService::Start()
{
	SC_HANDLE hManager = NULL;
	SC_HANDLE hService = NULL;
	BOOL ret = FALSE;
	SERVICE_STATUS sStatus;
	DWORD waited = 0;

	do
	{
		hManager = ::OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
		if (hManager == NULL)
			break;

		hService = ::OpenService(hManager, serviceName, SERVICE_START | SERVICE_QUERY_STATUS);
		if (hService == NULL)
			break;

		if (::QueryServiceStatus(hService, &sStatus) == FALSE)
			break;

		if (sStatus.dwCurrentState == SERVICE_RUNNING)
		{
			ret = TRUE;
			break;
		}

		if (::StartService(hService, NULL, NULL) == FALSE)
			break;

		while (1)
		{
			if (::QueryServiceStatus(hService, &sStatus) == FALSE)
				break;

			if (sStatus.dwCurrentState == SERVICE_RUNNING)
			{
				ret = TRUE;
				break;
			}

			if (waited >= sStatus.dwWaitHint)
				break;

			::Sleep(500);
			waited += 500;
		}
	}
	while (0);

	if (hService)
		::CloseServiceHandle(hService);

	if (hManager)
		::CloseServiceHandle(hManager);

	return ret;
}

//
// サービス停止
//
BOOL CWinService::Stop()
{
	SC_HANDLE hManager = NULL;
	SC_HANDLE hService = NULL;
	BOOL ret = FALSE;
	SERVICE_STATUS sStatus;
	DWORD waited = 0;

	do
	{
		hManager = ::OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
		if (hManager == NULL)
			break;

		hService = ::OpenService(hManager, serviceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
		if (hService == NULL)
			break;

		if (::QueryServiceStatus(hService, &sStatus) == FALSE)
			break;

		if (sStatus.dwCurrentState == SERVICE_STOPPED)
		{
			ret = TRUE;
			break;
		}

		if (::ControlService(hService, SERVICE_CONTROL_STOP, &sStatus) == FALSE)
			break;

		while (1)
		{
			if (::QueryServiceStatus(hService, &sStatus) == FALSE)
				break;

			if (sStatus.dwCurrentState == SERVICE_STOPPED)
			{
				ret = TRUE;
				break;
			}

			if (waited >= sStatus.dwWaitHint)
				break;

			::Sleep(500);
			waited += 500;
		}
	}
	while (0);

	if (hService)
		::CloseServiceHandle(hService);

	if (hManager)
		::CloseServiceHandle(hManager);

	return ret;
}

//
// サービス再起動
//
BOOL CWinService::Restart()
{
	if (Stop())
		return Start();
	return FALSE;
}

//
// サービス実行
//
BOOL CWinService::Run(LPSERVICE_MAIN_FUNCTIONW lpServiceProc)
{
	SERVICE_TABLE_ENTRY DispatchTable[] = { { serviceName, lpServiceProc }, { NULL, NULL } };
	return ::StartServiceCtrlDispatcher(DispatchTable);
}

//
// ServiceMainからサービス開始前に呼び出す
//
BOOL CWinService::RegisterService()
{
	// サービス停止用イベントを作成
	hServerStopEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL);
	if (hServerStopEvent == NULL)
		return FALSE;

	// SCMからの制御ハンドラを登録
	serviceStatusHandle = ::RegisterServiceCtrlHandlerEx(serviceName, ServiceCtrlHandler, NULL);
	if (serviceStatusHandle == 0)
	{
		::CloseHandle(hServerStopEvent);
		hServerStopEvent = NULL;
		return FALSE;
	}

	// 状態を開始中に設定
	serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	serviceStatus.dwCurrentState = SERVICE_START_PENDING;
	serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
	serviceStatus.dwWin32ExitCode = NO_ERROR;
	serviceStatus.dwServiceSpecificExitCode = 0;
	serviceStatus.dwCheckPoint = 1;
	serviceStatus.dwWaitHint = 30000;
	::SetServiceStatus(serviceStatusHandle, &serviceStatus);

	return TRUE;
}

//
// ServiceMainからサービス開始後呼出す(停止要求までreturnしない)
//
void CWinService::ServiceRunning()
{
	// 使い方間違ってる(RegisterService()を呼んでない)
	if (hServerStopEvent == NULL)
		return;

	// 状態を開始に設定
	serviceStatus.dwCurrentState = SERVICE_RUNNING;
	serviceStatus.dwCheckPoint = 0;
	serviceStatus.dwWaitHint = 0;
	::SetServiceStatus(serviceStatusHandle, &serviceStatus);

	// サービスに停止要求が送られてくるまで待機
	::WaitForSingleObject(hServerStopEvent, INFINITE);

	return;
}

//
// ServiceMainからサービス終了処理後呼び出す
//
void CWinService::ServiceStopped()
{
	if (hServerStopEvent)
	{
		//イベントクローズ
		::CloseHandle(hServerStopEvent);
		hServerStopEvent = NULL;

		//状態を停止に設定
		serviceStatus.dwCurrentState = SERVICE_STOPPED;
		serviceStatus.dwCheckPoint = 0;
		serviceStatus.dwWaitHint = 0;
		::SetServiceStatus(serviceStatusHandle, &serviceStatus);
	}
	return;
}

//
// サービスコントロールハンドラ処理
//
DWORD WINAPI CWinService::ServiceCtrlHandler(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext)
{
	switch (dwControl)
	{
	case SERVICE_CONTROL_STOP:
	{
		if (hServerStopEvent)
		{
			serviceStatus.dwWin32ExitCode = 0;
			serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
			serviceStatus.dwCheckPoint = 0;
			serviceStatus.dwWaitHint = 30000;
			::SetServiceStatus(serviceStatusHandle, &serviceStatus);
			// 停止イベントをトリガ
			::SetEvent(hServerStopEvent);
		}
		break;
	}

	case SERVICE_CONTROL_INTERROGATE:
		::SetServiceStatus(serviceStatusHandle, &serviceStatus);
		break;

	default:
		break;
	}
	return NO_ERROR;
}
