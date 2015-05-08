#include <locale.h>
#include "CWinService.cpp"

static void WINAPI ServiceMain(DWORD argc, LPTSTR *argv)
{
	CWinService *lpCWinService = CWinService::getInstance();
	if (lpCWinService->RegisterService() != TRUE)
		return;

	BOOL bWinsockInit = FALSE;
	do
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			break;
		bWinsockInit = TRUE;

		if (Init(NULL) != 0)
			break;

		HostInfo *phi = new HostInfo;
		phi->host = g_Host;
		phi->port = g_Port;
		g_hListenThread = CreateThread(NULL, 0, Listen, phi, 0, NULL);
		if (g_hListenThread)
		{
			lpCWinService->ServiceRunning();
			ShutdownInstances();	// g_hListenThreadはこの中でCloseHandle()される
		}
		else
			delete phi;
		CleanUp();	// ShutdownInstances()でg_LoadedDriverListにアクセスするスレッドは無くなっているはず
	} while (0);

	if (bWinsockInit)
		WSACleanup();

	lpCWinService->ServiceStopped();

	return;
}

static int RunOnCmd(HINSTANCE hInstance)
{
	if (Init(hInstance) != 0)
		return -1;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return -2;

	HostInfo *phi = new HostInfo;
	phi->host = g_Host;
	phi->port = g_Port;
	int ret = (int)Listen(phi);

	ShutdownInstances();
	CleanUp();

	WSACleanup();
	return ret;
}

static BOOL WINAPI HandlerRoutine(DWORD dwCtrlType)
{
	switch (dwCtrlType)
	{
	case CTRL_C_EVENT:
		g_ShutdownEvent.Set();
		return TRUE;
	default:
		return FALSE;
	}
}

int _tmain(int argc, _TCHAR *argv[], _TCHAR *envp[])
{
#if _DEBUG
	_CrtMemState ostate, nstate, dstate;
	_CrtMemCheckpoint(&ostate);
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	HANDLE hLogFile = NULL;
#endif

	int ret = 0;
	CWinService *lpCWinService = CWinService::getInstance();
	if (lpCWinService)
	{
		_tsetlocale(LC_ALL, _T(""));
		do
		{
			if (argc == 1)
			{
				// 引数なしで起動された
#if _DEBUG
				TCHAR szDrive[4];
				TCHAR szPath[MAX_PATH];
				TCHAR szLogFile[MAX_PATH + 16];
				GetModuleFileName(NULL, szPath, MAX_PATH);
				_tsplitpath_s(szPath, szDrive, 4, szPath, MAX_PATH, NULL, 0, NULL, 0);
				_tmakepath_s(szLogFile, MAX_PATH + 16, szDrive, szPath, _T("dbglog"), _T(".txt"));
				hLogFile = CreateFile(szLogFile, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
				SetFilePointer(hLogFile, 0, NULL, FILE_END);
				_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
				_CrtSetReportFile(_CRT_WARN, hLogFile);
				_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
				_CrtSetReportFile(_CRT_ERROR, hLogFile);
				_RPT0(_CRT_WARN, "--- PROCESS_START ---\n");
#endif
				if (lpCWinService->Run(ServiceMain))
				{
					// サービスだった
					break;
				}
				else
				{
					// サービスではない
					_tprintf(_T("コンソールモードで開始します...Ctrl+Cで終了\n"));
					SetConsoleCtrlHandler(HandlerRoutine, TRUE);
					ret = RunOnCmd(GetModuleHandle(NULL));
					switch (ret)
					{
					case -1:
						_tprintf(_T("iniファイルの読込に失敗しました\n"));
						break;
					case -2:
						_tprintf(_T("winsockの初期化に失敗しました\n"));
						break;
					case 1:
						_tprintf(_T("Hostアドレスの解決に失敗しました\n"));
						break;
					case 2:
						_tprintf(_T("bind()に失敗しました\n"));
						break;
					case 3:
						_tprintf(_T("listen()に失敗しました\n"));
						break;
					case 4:
						_tprintf(_T("accept()中にエラーが発生しました\n"));
						break;
					case 0:
						_tprintf(_T("終了します\n"));
						break;
					}
					break;
				}
			}
			else
			{
				// 引数あり
				BOOL done = FALSE;
				for (int i = 1; i < argc; i++)
				{
					if (_tcscmp(argv[i], _T("install")) == 0)
					{
						if (lpCWinService->Install())
							_tprintf(_T("Windowsサービスとして登録しました\n"));
						else
							_tprintf(_T("Windowsサービスとしての登録に失敗しました\n"));
						done = TRUE;
						break;
					}
					else if (_tcscmp(argv[i], _T("remove")) == 0)
					{
						if (lpCWinService->Remove())
							_tprintf(_T("Windowsサービスから削除しました\n"));
						else
							_tprintf(_T("Windowsサービスからの削除に失敗しました\n"));
						done = TRUE;
						break;
					}
					else if (_tcscmp(argv[i], _T("start")) == 0)
					{
						if (lpCWinService->Start())
							_tprintf(_T("Windowsサービスを起動しました\n"));
						else
							_tprintf(_T("Windowsサービスの起動に失敗しました\n"));
						done = TRUE;
						break;
					}
					else if (_tcscmp(argv[i], _T("stop")) == 0)
					{
						if (lpCWinService->Stop())
							_tprintf(_T("Windowsサービスを停止しました\n"));
						else
							_tprintf(_T("Windowsサービスの停止に失敗しました\n"));
						done = TRUE;
						break;
					}
					else if (_tcscmp(argv[i], _T("restart")) == 0)
					{
						if (lpCWinService->Restart())
							_tprintf(_T("Windowsサービスを再起動しました\n"));
						else
							_tprintf(_T("Windowsサービスの再起動に失敗しました\n"));
						done = TRUE;
						break;
					}
				}
				if (done)
					break;
			}
			// Usage表示
			_tprintf(_T("Usage: %s <command>\n")
				_T("コマンド\n")
				_T("  install    Windowsサービスとして登録します\n")
				_T("  remove     Windowsサービスから削除します\n")
				_T("  start      Windowsサービスを起動します\n")
				_T("  stop       Windowsサービスを停止します\n")
				_T("  restart    Windowsサービスを再起動します\n")
				_T("\n")
				_T("引数なしで起動された場合、コンソールモードで動作します\n"),
				argv[0]);
		} while (0);
	}
	else
	{
		_tprintf(_T("Windowsサービスクラスの初期化に失敗しました\n"));
		ret = -1;
	}

#if _DEBUG
	_CrtMemCheckpoint(&nstate);
	if (_CrtMemDifference(&dstate, &ostate, &nstate))
	{
		_CrtMemDumpStatistics(&dstate);
		_CrtMemDumpAllObjectsSince(&ostate);
	}
	if (hLogFile)
	{
		_RPT0(_CRT_WARN, "--- PROCESS_END ---\n");
		CloseHandle(hLogFile);
	}
#endif

	return ret;
}
