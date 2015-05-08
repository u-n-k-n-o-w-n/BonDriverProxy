#ifndef __CWINSERVICE_H__
#define __CWINSERVICE_H__

// SERVICE_WIN32_OWN_PROCESSのサービスを操作する為のユーティリティクラス
class CWinService
{
	static CWinService This;
	static SERVICE_STATUS serviceStatus;
	static SERVICE_STATUS_HANDLE serviceStatusHandle;
	// 終了イベント
	static HANDLE hServerStopEvent;
	// サービス名称
	TCHAR serviceName[BUFSIZ];
	// サービス実体パス
	TCHAR serviceExePath[BUFSIZ];

	CWinService();
	virtual ~CWinService();

	// サービスコントロールハンドラ
	static DWORD WINAPI ServiceCtrlHandler(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext);

public:
	static CWinService *getInstance(){ return &This; };

	// インストールとアンインストール
	BOOL Install();
	BOOL Remove();

	// 起動・停止・再起動
	BOOL Start();
	BOOL Stop();
	BOOL Restart();

	// 実行
	BOOL Run(LPSERVICE_MAIN_FUNCTIONW lpServiceProc);

	// サービスメインから呼び出す手続き関数
	BOOL RegisterService();
	void ServiceRunning();
	void ServiceStopped();
};

#endif // __CWINSERVICE_H__
