#define _CRT_SECURE_NO_WARNINGS
#include "BonDriverProxy.h"

static std::list<cProxyClient *> InstanceList;
static cCriticalSection Lock_Global;

cProxyClient::cProxyClient() : m_Error(TRUE, FALSE)
{
	m_s = INVALID_SOCKET;
	m_dwBufPos = 0;
	m_pRes = NULL;
	m_LastBuff = NULL;
	::memset(m_pBuf, 0, sizeof(m_pBuf));
	m_bBonDriver = m_bTuner = m_bRereased = FALSE;
	m_fSignalLevel = 0;
	m_dwSpace = m_dwChannel = 0xff;
//	m_hThread = NULL;
	m_iEndCount = -1;
}

cProxyClient::~cProxyClient()
{
	if (!m_bRereased)
	{
		if (m_bTuner)
			CloseTuner();
		makePacket(eRelease);
	}

	m_Error.Set();

	if (m_iEndCount != -1)
		SleepLock(3);

//	if (m_hThread != NULL)
//		::WaitForSingleObject(m_hThread, INFINITE);

	{
		LOCK(m_writeLock);
		for (int i = 0; i < 8; i++)
		{
			if (m_pBuf[i] != NULL)
				delete[] m_pBuf[i];
		}
		::memset(m_pBuf, 0, sizeof(m_pBuf));
		TsFlush();
		if (m_LastBuff != NULL)
		{
			delete m_LastBuff;
			m_LastBuff = NULL;
		}
	}

	if (m_s != INVALID_SOCKET)
		::closesocket(m_s);
}

DWORD WINAPI cProxyClient::ProcessEntry(LPVOID pv)
{
	cProxyClient *pProxy = static_cast<cProxyClient *>(pv);
	DWORD ret = pProxy->Process();
	pProxy->m_iEndCount++;
	return ret;
}

DWORD cProxyClient::Process()
{
	HANDLE hThread[2];
	hThread[0] = ::CreateThread(NULL, 0, cProxyClient::Sender, this, 0, NULL);
	if (hThread[0] == NULL)
	{
		m_Error.Set();
		m_SingleShot.Set();
		return 1;
	}

	hThread[1] = ::CreateThread(NULL, 0, cProxyClient::Receiver, this, 0, NULL);
	if (hThread[1] == NULL)
	{
		m_Error.Set();
		::WaitForSingleObject(hThread[0], INFINITE);
		::CloseHandle(hThread[0]);
		m_SingleShot.Set();
		return 2;
	}

	m_iEndCount = 0;
	m_SingleShot.Set();

	HANDLE h[2] = { m_Error, m_fifoRecv.GetEventHandle() };
	while (1)
	{
		DWORD dwRet = ::WaitForMultipleObjects(2, h, FALSE, INFINITE);
		switch (dwRet)
		{
		case WAIT_OBJECT_0:
			goto end;

		case WAIT_OBJECT_0 + 1:
		{
			cPacketHolder *pPh;
			m_fifoRecv.Pop(&pPh);
			switch (pPh->GetCommand())
			{
			case eSelectBonDriver:
			case eCreateBonDriver:
			case eOpenTuner:
			case ePurgeTsStream:
			case eSetLnbPower:
			{
				LOCK(m_readLock);
				if (pPh->GetBodyLength() != sizeof(BYTE))
					m_bRes = FALSE;
				else
					m_bRes = pPh->m_pPacket->payload[0];
				m_SingleShot.Set();
				break;
			}

			case eGetTsStream:
				if (pPh->GetBodyLength() >= (sizeof(DWORD) * 2))
				{
					DWORD dwSize = ::ntohl(*(DWORD *)(pPh->m_pPacket->payload));
					// 変なパケットは廃棄(正規のサーバに繋いでいる場合は来る事はないハズ)
					if ((pPh->GetBodyLength() - (sizeof(DWORD) * 2)) == dwSize)
					{
						union {
							DWORD dw;
							float f;
						} u;
						u.dw = ::ntohl(*(DWORD *)&(pPh->m_pPacket->payload[sizeof(DWORD)]));
						m_fSignalLevel = u.f;

						TS_DATA *pData = new TS_DATA();
						pData->dwSize = dwSize;
						pData->pbBuff = new BYTE[dwSize];
						::memcpy(pData->pbBuff, &(pPh->m_pPacket->payload[sizeof(DWORD) * 2]), dwSize);
						m_fifoTS.Push(pData);
					}
				}
				break;

			case eEnumTuningSpace:
			case eEnumChannelName:
			{
				LOCK(m_writeLock);
				if (m_dwBufPos >= 8)
					m_dwBufPos = 0;
				if (m_pBuf[m_dwBufPos])
					delete[] m_pBuf[m_dwBufPos];
				if (pPh->GetBodyLength() == sizeof(TCHAR))
					m_pBuf[m_dwBufPos] = NULL;
				else
				{
					m_pBuf[m_dwBufPos] = (TCHAR *)(new BYTE[pPh->GetBodyLength()]);
					::_tcscpy(m_pBuf[m_dwBufPos], (TCHAR *)(pPh->m_pPacket->payload));
				}
				{
					LOCK(m_readLock);
					m_pRes = m_pBuf[m_dwBufPos++];
				}
				m_SingleShot.Set();
				break;
			}

			case eSetChannel2:
			case eGetTotalDeviceNum:
			case eGetActiveDeviceNum:
			{
				LOCK(m_readLock);
				if (pPh->GetBodyLength() != sizeof(DWORD))
					m_dwRes = 0;
				else
					m_dwRes = ::ntohl(*(DWORD *)(pPh->m_pPacket->payload));
				m_SingleShot.Set();
				break;
			}

			default:
				break;
			}
			delete pPh;
			break;
		}

		default:
			// 何かのエラー
			m_Error.Set();
			goto end;
		}
	}
end:

//	Sender / Receiver共にExitThread()が呼ばれてるのにハンドルがシグナル状態にならない。謎。
//	理由知ってる人がいたら教えて下さい(;´Д`)
//	::WaitForMultipleObjects(2, hThread, TRUE, INFINITE);

	SleepLock(2);	//	しょうがないのでスリープロックで対応…
	::CloseHandle(hThread[0]);
	::CloseHandle(hThread[1]);
	return 0;
}

int cProxyClient::ReceiverHelper(char *pDst, int left)
{
	int len, ret;
	fd_set rd;
	timeval tv;

	tv.tv_sec = 1;
	tv.tv_usec = 0;
	while (left > 0)
	{
		if (m_Error.IsSet())
			return -1;

		FD_ZERO(&rd);
		FD_SET(m_s, &rd);
		if ((len = ::select((int)(m_s + 1), &rd, NULL, NULL, &tv)) == SOCKET_ERROR)
		{
			ret = -2;
			goto err;
		}

		if (len == 0)
			continue;

		if ((len = ::recv(m_s, pDst, left, 0)) == SOCKET_ERROR)
		{
			ret = -3;
			goto err;
		}
		else if (len == 0)
		{
			ret = -4;
			goto err;
		}
		left -= len;
		pDst += len;
	}
	return 0;
err:
	m_Error.Set();
	return ret;
}

DWORD WINAPI cProxyClient::Receiver(LPVOID pv)
{
	cProxyClient *pProxy = static_cast<cProxyClient *>(pv);
	int left;
	char *p;
	DWORD ret;
	cPacketHolder *pPh = NULL;
	const DWORD TsPacketBufSize = g_TsPacketBufSize;

	while (1)
	{
		pPh = new cPacketHolder(16);
		left = sizeof(stPacketHead);
		p = (char *)&(pPh->m_pPacket->head);
		if (pProxy->ReceiverHelper(p, left) != 0)
		{
			ret = 201;
			goto end;
		}

		if (!pPh->IsValid())
		{
			pProxy->m_Error.Set();
			ret = 202;
			goto end;
		}

		left = (int)pPh->GetBodyLength();
		if (left == 0)
		{
			pProxy->m_fifoRecv.Push(pPh);
			continue;
		}

		if ((!pPh->IsTS() && (left > 512)) || left < 0)
		{
			pProxy->m_Error.Set();
			ret = 203;
			goto end;
		}

		if (left >= 16)
		{
			if ((DWORD)left > (TsPacketBufSize + (sizeof(DWORD) * 2)))
			{
				pProxy->m_Error.Set();
				ret = 204;
				goto end;
			}
			cPacketHolder *pTmp = new cPacketHolder(left);
			pTmp->m_pPacket->head = pPh->m_pPacket->head;
			delete pPh;
			pPh = pTmp;
		}

		p = (char *)(pPh->m_pPacket->payload);
		if (pProxy->ReceiverHelper(p, left) != 0)
		{
			ret = 205;
			goto end;
		}

		pProxy->m_fifoRecv.Push(pPh);
	}
end:
	if (pPh)
		delete pPh;
	pProxy->m_iEndCount++;
	return ret;
}

void cProxyClient::makePacket(enumCommand eCmd)
{
	cPacketHolder *p = new cPacketHolder(eCmd, 0);
	m_fifoSend.Push(p);
}

void cProxyClient::makePacket(enumCommand eCmd, LPCSTR str)
{
	register size_t size = (::strlen(str) + 1);
	cPacketHolder *p = new cPacketHolder(eCmd, size);
	::memcpy(p->m_pPacket->payload, str, size);
	m_fifoSend.Push(p);
}

void cProxyClient::makePacket(enumCommand eCmd, BOOL b)
{
	cPacketHolder *p = new cPacketHolder(eCmd, sizeof(BYTE));
	p->m_pPacket->payload[0] = (BYTE)b;
	m_fifoSend.Push(p);
}

void cProxyClient::makePacket(enumCommand eCmd, DWORD dw)
{
	cPacketHolder *p = new cPacketHolder(eCmd, sizeof(DWORD));
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos = ::htonl(dw);
	m_fifoSend.Push(p);
}

void cProxyClient::makePacket(enumCommand eCmd, DWORD dw1, DWORD dw2)
{
	cPacketHolder *p = new cPacketHolder(eCmd, sizeof(DWORD) * 2);
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos++ = ::htonl(dw1);
	*pos = ::htonl(dw2);
	m_fifoSend.Push(p);
}

void cProxyClient::makePacket(enumCommand eCmd, DWORD dw1, DWORD dw2, BOOL b)
{
	cPacketHolder *p = new cPacketHolder(eCmd, (sizeof(DWORD) * 2) + sizeof(BYTE));
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos++ = ::htonl(dw1);
	*pos++ = ::htonl(dw2);
	*(BYTE *)pos = (BYTE)b;
	m_fifoSend.Push(p);
}

DWORD WINAPI cProxyClient::Sender(LPVOID pv)
{
	cProxyClient *pProxy = static_cast<cProxyClient *>(pv);
	DWORD ret;
	HANDLE h[2] = { pProxy->m_Error, pProxy->m_fifoSend.GetEventHandle() };
	while (1)
	{
		DWORD dwRet = ::WaitForMultipleObjects(2, h, FALSE, INFINITE);
		switch (dwRet)
		{
		case WAIT_OBJECT_0:
			ret = 101;
			goto end;

		case WAIT_OBJECT_0 + 1:
		{
			cPacketHolder *pPh;
			pProxy->m_fifoSend.Pop(&pPh);
			int left = (int)pPh->m_Size;
			char *p = (char *)(pPh->m_pPacket);
			while (left > 0)
			{
				int len = ::send(pProxy->m_s, p, left, 0);
				if (len == SOCKET_ERROR)
				{
					pProxy->m_Error.Set();
					break;
				}
				left -= len;
				p += len;
			}
			delete pPh;
			break;
		}

		default:
			// 何かのエラー
			pProxy->m_Error.Set();
			ret = 102;
			goto end;
		}
	}
end:
	pProxy->m_iEndCount++;
	return ret;
}

BOOL cProxyClient::SelectBonDriver()
{
	{
		LOCK(Lock_Global);
		makePacket(eSelectBonDriver, g_BonDriver);
	}
	m_SingleShot.Wait();
	{
		LOCK(m_readLock);
		return m_bRes;
	}
}

BOOL cProxyClient::CreateBonDriver()
{
	makePacket(eCreateBonDriver);
	m_SingleShot.Wait();
	{
		LOCK(m_readLock);
		if (m_bRes)
			m_bBonDriver = TRUE;
		return m_bRes;
	}
}

const BOOL cProxyClient::OpenTuner(void)
{
	if (!m_bBonDriver)
		return FALSE;
	makePacket(eOpenTuner);
	m_SingleShot.Wait();
	{
		LOCK(m_readLock);
		if (m_bRes)
			m_bTuner = TRUE;
		return m_bRes;
	}
}

void cProxyClient::CloseTuner(void)
{
	if (!m_bTuner)
		return;

	makePacket(eCloseTuner);
	m_bTuner = FALSE;
	m_fSignalLevel = 0;
	m_dwSpace = m_dwChannel = 0xff;
	{
		LOCK(m_writeLock);
		m_dwBufPos = 0;
		m_pRes = NULL;
		for (int i = 0; i < 8; i++)
		{
			if (m_pBuf[i] != NULL)
				delete[] m_pBuf[i];
		}
		::memset(m_pBuf, 0, sizeof(m_pBuf));
	}
}

const BOOL cProxyClient::SetChannel(const BYTE bCh)
{
	return TRUE;
}

const float cProxyClient::GetSignalLevel(void)
{
	return m_fSignalLevel;
}

const DWORD cProxyClient::WaitTsStream(const DWORD dwTimeOut)
{
	if (!m_bTuner)
		return WAIT_ABANDONED;

	HANDLE h[2] = { m_fifoTS.GetEventHandle(), m_Error };
	DWORD ret = ::WaitForMultipleObjects(2, h, FALSE, (dwTimeOut) ? dwTimeOut : INFINITE);
	switch (ret)
	{
	case WAIT_ABANDONED:
	case WAIT_OBJECT_0 + 1:
		return WAIT_ABANDONED;

	case WAIT_OBJECT_0:
	case WAIT_TIMEOUT:
		break;

	default:
		return WAIT_FAILED;
	}
	return ret;
}

const DWORD cProxyClient::GetReadyCount(void)
{
	if (!m_bTuner)
		return 0;
	return (DWORD)m_fifoTS.Size();
}

const BOOL cProxyClient::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!m_bTuner)
		return FALSE;
	BYTE *pSrc;
	if (GetTsStream(&pSrc, pdwSize, pdwRemain))
	{
		if (*pdwSize)
			::memcpy(pDst, pSrc, *pdwSize);
		return TRUE;
	}
	return FALSE;
}

const BOOL cProxyClient::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!m_bTuner)
		return FALSE;

	BOOL b;
#if _DEBUG && DETAILLOG
	static DWORD Counter = 0;
#endif
	{
		LOCK(m_writeLock);
		if (m_fifoTS.Size() != 0)
		{
			if (m_LastBuff != NULL)
			{
				delete m_LastBuff;
				m_LastBuff = NULL;
			}
			m_fifoTS.Pop(&m_LastBuff);
			*ppDst = m_LastBuff->pbBuff;
			*pdwSize = m_LastBuff->dwSize;
			*pdwRemain = (DWORD)m_fifoTS.Size();
			b = TRUE;
#if _DEBUG && DETAILLOG
			_RPT3(_CRT_WARN, "GetTsStream() : %u : *pdwSize[%x] *pdwRemain[%d]\n", Counter++, *pdwSize, *pdwRemain);
#endif
		}
		else
		{
			*pdwSize = 0;
			*pdwRemain = 0;
			b = FALSE;
		}
	}
	return b;
}

void cProxyClient::PurgeTsStream(void)
{
	if (!m_bTuner)
		return;
	makePacket(ePurgeTsStream);
	m_SingleShot.Wait();
	BOOL b;
	{
		LOCK(m_readLock);
		b = m_bRes;
	}
	if (b)
	{
		LOCK(m_writeLock);
		TsFlush();
	}
}

void cProxyClient::Release(void)
{
	if (m_bTuner)
		CloseTuner();
	makePacket(eRelease);
	m_bRereased = TRUE;
	{
		LOCK(Lock_Global);
		InstanceList.remove(this);
	}
	delete this;
}

LPCTSTR cProxyClient::GetTunerName(void)
{
	return _T(TUNER_NAME);
}

const BOOL cProxyClient::IsTunerOpening(void)
{
	return FALSE;
}

LPCTSTR cProxyClient::EnumTuningSpace(const DWORD dwSpace)
{
	if (!m_bTuner)
		return NULL;
	makePacket(eEnumTuningSpace, dwSpace);
	m_SingleShot.Wait();
	{
		LOCK(m_readLock);
		return m_pRes;
	}
}

LPCTSTR cProxyClient::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	if (!m_bTuner)
		return NULL;
	makePacket(eEnumChannelName, dwSpace, dwChannel);
	m_SingleShot.Wait();
	{
		LOCK(m_readLock);
		return m_pRes;
	}
}

const BOOL cProxyClient::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	if (!m_bTuner)
		return FALSE;
	if ((m_dwSpace == dwSpace) && (m_dwChannel == dwChannel))
		return TRUE;
	makePacket(eSetChannel2, dwSpace, dwChannel, g_ChannelLock);
	m_SingleShot.Wait();
	DWORD dw;
	{
		LOCK(m_readLock);
		dw = m_dwRes;
	}
	BOOL b;
	switch (dw)
	{
	case 0x00:	// 成功
	{
		LOCK(m_writeLock);
		TsFlush();
		m_dwSpace = dwSpace;
		m_dwChannel = dwChannel;
	}
	case 0x01:	// fall-through / チャンネルロックされてる
		b = TRUE;
		break;
	default:
		b = FALSE;
		break;
	}
	return b;
}

const DWORD cProxyClient::GetCurSpace(void)
{
	return m_dwSpace;
}

const DWORD cProxyClient::GetCurChannel(void)
{
	return m_dwChannel;
}

const DWORD cProxyClient::GetTotalDeviceNum(void)
{
	if (!m_bTuner)
		return 0;
	makePacket(eGetTotalDeviceNum);
	m_SingleShot.Wait();
	{
		LOCK(m_readLock);
		return m_dwRes;
	}
}

const DWORD cProxyClient::GetActiveDeviceNum(void)
{
	if (!m_bTuner)
		return 0;
	makePacket(eGetActiveDeviceNum);
	m_SingleShot.Wait();
	{
		LOCK(m_readLock);
		return m_dwRes;
	}
}

const BOOL cProxyClient::SetLnbPower(const BOOL bEnable)
{
	if (!m_bTuner)
		return FALSE;
	makePacket(eSetLnbPower, bEnable);
	m_SingleShot.Wait();
	{
		LOCK(m_readLock);
		return m_bRes;
	}
}

SOCKET Connect(char *host, unsigned short port)
{
	SOCKADDR_IN server;
	LPHOSTENT he;
	SOCKET sock;
	int i;
	unsigned long bf;
	fd_set wd;
	timeval tv;

	if (g_UseMagicPacket)
	{
		char sendbuf[128];
		memset(sendbuf, 0xff, 6);
		for (i = 1; i <= 16; i++)
			memcpy(&sendbuf[i * 6], g_TargetMac, 6);

		sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (sock == INVALID_SOCKET)
			return INVALID_SOCKET;

		BOOL opt = TRUE;
		if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&opt, sizeof(opt)) != 0)
		{
			closesocket(sock);
			return INVALID_SOCKET;
		}

		memset((char *)&server, 0, sizeof(server));
		server.sin_family = AF_INET;
		server.sin_addr.s_addr = inet_addr(g_TargetHost);
		if (server.sin_addr.s_addr == INADDR_NONE)
		{
			he = gethostbyname(g_TargetHost);
			if (he == NULL)
			{
				closesocket(sock);
				return INVALID_SOCKET;
			}
			memcpy(&(server.sin_addr), *(he->h_addr_list), he->h_length);
		}
		server.sin_port = htons(g_TargetPort);
		int ret = sendto(sock, sendbuf, 102, 0, (LPSOCKADDR)&server, sizeof(server));
		closesocket(sock);
		if (ret != 102)
			return INVALID_SOCKET;
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == INVALID_SOCKET)
		return INVALID_SOCKET;
	memset((char *)&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = inet_addr(host);
	if (server.sin_addr.s_addr == INADDR_NONE)
	{
		he = gethostbyname(host);
		if (he == NULL)
		{
			closesocket(sock);
			return INVALID_SOCKET;
		}
		memcpy(&(server.sin_addr), *(he->h_addr_list), he->h_length);
	}
	server.sin_port = htons(port);
	bf = TRUE;
	ioctlsocket(sock, FIONBIO, &bf);
	tv.tv_sec = g_ConnectTimeOut;
	tv.tv_usec = 0;
	FD_ZERO(&wd);
	FD_SET(sock, &wd);
	connect(sock, (LPSOCKADDR)&server, sizeof(server));
	if ((i = select((int)(sock + 1), 0, &wd, 0, &tv)) == SOCKET_ERROR)
	{
		closesocket(sock);
		return INVALID_SOCKET;
	}
	if (i == 0)
	{
		closesocket(sock);
		return INVALID_SOCKET;
	}
	bf = FALSE;
	ioctlsocket(sock, FIONBIO, &bf);
	return sock;
}

extern "C" __declspec(dllexport) IBonDriver *CreateBonDriver()
{
	SOCKET s = Connect(g_Host, g_Port);
	if (s == INVALID_SOCKET)
		return NULL;

	cProxyClient *pProxy = new cProxyClient();
	pProxy->setSocket(s);
	HANDLE hThread = ::CreateThread(NULL, 0, cProxyClient::ProcessEntry, pProxy, 0, NULL);
	if (hThread == NULL)
		goto err;
//	pProxy->setThreadHandle(hThread);

	pProxy->WaitSingleShot();
	if (pProxy->IsError())
		goto err;

	if (!pProxy->SelectBonDriver())
		goto err;

	if (pProxy->CreateBonDriver())
	{
		LOCK(Lock_Global);
		InstanceList.push_back(pProxy);
		return pProxy;
	}

err:
	delete pProxy;
	return NULL;
}

extern "C" __declspec(dllexport) BOOL SetBonDriver(LPCSTR p)
{
	LOCK(Lock_Global);
	if (strlen(p) >= sizeof(g_BonDriver))
		return FALSE;
	strcpy(g_BonDriver, p);
	return TRUE;
}

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
#if _DEBUG
	static HANDLE hLogFile;
#endif
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
	{
#if _DEBUG
		TCHAR buf[64];
		wsprintf(buf, _T("dbglog_dll_%u.txt"), ::GetTickCount());
		hLogFile = CreateFile(buf, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
		_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_WARN, hLogFile);
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(_CRT_ERROR, hLogFile);
		_RPT0(_CRT_WARN, "--- DLL_PROCESS_ATTACH ---\n");
#endif
		if (Init(hinstDLL) != 0)
			return FALSE;

		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return FALSE;
		break;
	}

	case DLL_PROCESS_DETACH:
	{
		{
			LOCK(Lock_Global);
			while (!InstanceList.empty())
			{
				cProxyClient *pProxy = InstanceList.front();
				InstanceList.pop_front();
				delete pProxy;
			}
		}
		WSACleanup();
#if _DEBUG
		_RPT0(_CRT_WARN, "--- DLL_PROCESS_DETACH ---\n");
//		CloseHandle(hLogFile);
#endif
		break;
	}
	}
	return TRUE;
}
