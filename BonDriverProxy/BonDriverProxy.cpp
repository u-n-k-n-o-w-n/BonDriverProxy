#define _CRT_SECURE_NO_WARNINGS
#include "BonDriverProxy.h"

#define STRICT_LOCK

#if _DEBUG
#define DETAILLOG	0
static cProxyServer *debug;
#endif

static int Init(HMODULE hModule)
{
	char szIniPath[MAX_PATH + 16] = { '\0' };
	GetModuleFileNameA(hModule, szIniPath, MAX_PATH);
	char *p = strrchr(szIniPath, '.');
	if (!p)
		return -1;
	p++;
	strcpy(p, "ini");

	HANDLE hFile = CreateFileA(szIniPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return -2;
	CloseHandle(hFile);

	GetPrivateProfileStringA("OPTION", "ADDRESS", "127.0.0.1", g_Host, sizeof(g_Host), szIniPath);
	GetPrivateProfileStringA("OPTION", "PORT", "1192", g_Port, sizeof(g_Port), szIniPath);
	g_SandBoxedRelease = GetPrivateProfileIntA("OPTION", "SANDBOXED_RELEASE", 0, szIniPath);

	g_PacketFifoSize = GetPrivateProfileIntA("SYSTEM", "PACKET_FIFO_SIZE", 64, szIniPath);
	g_TsPacketBufSize = GetPrivateProfileIntA("SYSTEM", "TSPACKET_BUFSIZE", (188 * 1024), szIniPath);

	return 0;
}

cProxyServer::cProxyServer() : m_Error(TRUE, FALSE)
{
	m_s = INVALID_SOCKET;
	m_hModule = NULL;
	m_pIBon = m_pIBon2 = m_pIBon3 = NULL;
	m_strBonDriver[0] = '\0';
	m_bTunerOpen = m_bChannelLock = FALSE;
	m_hTsRead = NULL;
	m_pTsReceiversList = NULL;
	m_pStopTsRead = NULL;
	m_pTsLock = NULL;
	m_ppos = NULL;
}

cProxyServer::~cProxyServer()
{
	LOCK(g_Lock);
	BOOL bRelease = TRUE;
	std::list<cProxyServer *>::iterator it = g_InstanceList.begin();
	while (it != g_InstanceList.end())
	{
		if (*it == this)
			g_InstanceList.erase(it++);
		else
		{
			if ((m_hModule != NULL) && (m_hModule == (*it)->m_hModule))
				bRelease = FALSE;
			++it;
		}
	}
	if (bRelease)
	{
		if (m_hTsRead)
		{
			*m_pStopTsRead = TRUE;
			::WaitForSingleObject(m_hTsRead, INFINITE);
			::CloseHandle(m_hTsRead);
			delete m_pTsReceiversList;
			delete m_pStopTsRead;
			delete m_pTsLock;
			delete m_ppos;
		}

		Release();

		if (m_hModule)
			::FreeLibrary(m_hModule);
	}
	else
	{
		if (m_hTsRead)
		{
			{
				LOCK(*m_pTsLock);
				it = m_pTsReceiversList->begin();
				while (it != m_pTsReceiversList->end())
				{
					if (*it == this)
					{
						m_pTsReceiversList->erase(it);
						break;
					}
					++it;
				}
			}
			// 可能性は低いがゼロではない…
			if (m_pTsReceiversList->empty())
			{
				*m_pStopTsRead = TRUE;
				::WaitForSingleObject(m_hTsRead, INFINITE);
				::CloseHandle(m_hTsRead);
				delete m_pTsReceiversList;
				delete m_pStopTsRead;
				delete m_pTsLock;
				delete m_ppos;
			}
		}
	}

	if (m_s != INVALID_SOCKET)
		::closesocket(m_s);
}

DWORD WINAPI cProxyServer::Reception(LPVOID pv)
{
	cProxyServer *pProxy = static_cast<cProxyServer *>(pv);
	DWORD ret = pProxy->Process();
#if _DEBUG
	debug = NULL;
#endif
	delete pProxy;
	return ret;
}

DWORD cProxyServer::Process()
{
	HANDLE hThread[2];
	hThread[0] = ::CreateThread(NULL, 0, cProxyServer::Sender, this, 0, NULL);
	if (hThread[0] == NULL)
		return 1;

	hThread[1] = ::CreateThread(NULL, 0, cProxyServer::Receiver, this, 0, NULL);
	if (hThread[1] == NULL)
	{
		m_Error.Set();
		::WaitForSingleObject(hThread[0], INFINITE);
		::CloseHandle(hThread[0]);
		return 2;
	}

#if _DEBUG
	debug = this;
#endif

	HANDLE h[2] = { m_Error, m_fifoRecv.GetEventHandle() };
	for (;;)
	{
		DWORD dwRet = ::WaitForMultipleObjects(2, h, FALSE, INFINITE);
		switch (dwRet)
		{
		case WAIT_OBJECT_0:
			goto end;

		case WAIT_OBJECT_0 + 1:
		{
#ifdef STRICT_LOCK
			LOCK(g_Lock);
#endif
			cPacketHolder *pPh;
			m_fifoRecv.Pop(&pPh);
			switch (pPh->GetCommand())
			{
			case eSelectBonDriver:
			{
				if (pPh->GetBodyLength() <= sizeof(char))
					makePacket(eSelectBonDriver, FALSE);
				else
				{
					BOOL bFind = FALSE;
#ifndef STRICT_LOCK
					LOCK(g_Lock);
#endif
					for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
					{
						if (::strcmp((LPCSTR)(pPh->m_pPacket->payload), (*it)->m_strBonDriver) == 0)
						{
							bFind = TRUE;
							m_hModule = (*it)->m_hModule;
							::strcpy(m_strBonDriver, (*it)->m_strBonDriver);
							m_pIBon = (*it)->m_pIBon;	// (*it)->m_pIBonがNULLの可能性はゼロではない
							m_pIBon2 = (*it)->m_pIBon2;
							m_pIBon3 = (*it)->m_pIBon3;
							break;
						}
					}
					BOOL bSuccess;
					if (!bFind)
					{
						bSuccess = SelectBonDriver((LPCSTR)(pPh->m_pPacket->payload));
						if (bSuccess)
						{
							g_InstanceList.push_back(this);
							::strcpy_s(m_strBonDriver, (LPCSTR)(pPh->m_pPacket->payload));
						}
					}
					else
					{
						g_InstanceList.push_back(this);
						bSuccess = TRUE;
					}
					makePacket(eSelectBonDriver, bSuccess);
				}
				break;
			}

			case eCreateBonDriver:
			{
				if (m_pIBon == NULL)
				{
					BOOL bFind = FALSE;
					BOOL bLoop = FALSE;
					{
#ifndef STRICT_LOCK
						LOCK(g_Lock);
#endif
						for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
						{
							if (*it == this)
								continue;
							if (m_hModule == (*it)->m_hModule)
							{
								if ((*it)->m_pIBon != NULL)
								{
									bFind = TRUE;	// ここに来るのはかなりのレアケースのハズ
									m_pIBon = (*it)->m_pIBon;
									m_pIBon2 = (*it)->m_pIBon2;
									m_pIBon3 = (*it)->m_pIBon3;
									break;
								}
								else
								{
									// ここに来るのは上より更にレアケース、あるいはクライアントが
									// BonDriver_Proxy.dllを要求し、サーバ側のBonDriver_Proxy.dllも
									// 同じサーバに対して自分自身を要求する無限ループ状態だけのハズ
									// なお、STRICT_LOCKが定義してある場合は、そもそもデッドロックを
									// 起こすので、後者の状況は発生しない

									// 気休めの雑なチェック
									if (!::_memicmp(m_strBonDriver, "BonDriver_Proxy", 15))
									{
										bLoop = TRUE;
										break;
									}

									// 無限ループ状態以外の場合は一応リストの最後まで検索してみて、
									// それでも見つからなかったらCreateBonDriver()をやらせてみる
								}
							}
						}
					}
					if (!bFind && !bLoop)
					{
						if ((CreateBonDriver() != NULL) && (m_pIBon2 != NULL))
							makePacket(eCreateBonDriver, TRUE);
						else
						{
							makePacket(eCreateBonDriver, FALSE);
							m_Error.Set();
						}
					}
					else
					{
						if (!bLoop)
							makePacket(eCreateBonDriver, TRUE);
						else
						{
							makePacket(eCreateBonDriver, FALSE);
							m_Error.Set();
						}
					}
				}
				else
					makePacket(eCreateBonDriver, TRUE);
				break;
			}

			case eOpenTuner:
			{
				BOOL bFind = FALSE;
				{
#ifndef STRICT_LOCK
					LOCK(g_Lock);
#endif
					for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
					{
						if (*it == this)
							continue;
						if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
						{
							if ((*it)->m_bTunerOpen)
							{
								bFind = TRUE;
								m_bTunerOpen = TRUE;
								break;
							}
						}
					}
				}
				if (!bFind)
					m_bTunerOpen = OpenTuner();
				makePacket(eOpenTuner, m_bTunerOpen);
				break;
			}

			case eCloseTuner:
			{
				BOOL bFind = FALSE;
				{
#ifndef STRICT_LOCK
					LOCK(g_Lock);
#endif
					for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
					{
						if (*it == this)
							continue;
						if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
						{
							if ((*it)->m_bTunerOpen)
							{
								bFind = TRUE;
								break;
							}
						}
					}
				}
				if (!bFind)
				{
					if (m_hTsRead)
					{
						*m_pStopTsRead = TRUE;
						::WaitForSingleObject(m_hTsRead, INFINITE);
						::CloseHandle(m_hTsRead);
						m_hTsRead = NULL;
						m_pTsReceiversList->clear();
						delete m_pTsReceiversList;
						m_pTsReceiversList = NULL;
						delete m_pStopTsRead;
						m_pStopTsRead = NULL;
						delete m_pTsLock;
						m_pTsLock = NULL;
						delete m_ppos;
						m_ppos = NULL;
					}
					CloseTuner();
				}
				m_bTunerOpen = FALSE;
				break;
			}

			case ePurgeTsStream:
			{
				if (m_hTsRead && m_bChannelLock)
				{
					LOCK(*m_pTsLock);
					PurgeTsStream();
					*m_ppos = 0;
					makePacket(ePurgeTsStream, TRUE);
				}
				else
					makePacket(ePurgeTsStream, FALSE);
				break;
			}

			case eRelease:
				m_Error.Set();
				break;

			case eEnumTuningSpace:
			{
				if (pPh->GetBodyLength() != sizeof(DWORD))
					makePacket(eEnumTuningSpace, _T(""));
				else
				{
					LPCTSTR p = EnumTuningSpace(::ntohl(*(DWORD *)(pPh->m_pPacket->payload)));
					if (p)
						makePacket(eEnumTuningSpace, p);
					else
						makePacket(eEnumTuningSpace, _T(""));
				}
				break;
			}

			case eEnumChannelName:
			{
				if (pPh->GetBodyLength() != (sizeof(DWORD) * 2))
					makePacket(eEnumChannelName, _T(""));
				else
				{
					LPCTSTR p = EnumChannelName(::ntohl(*(DWORD *)(pPh->m_pPacket->payload)), ::ntohl(*(DWORD *)&(pPh->m_pPacket->payload[sizeof(DWORD)])));
					if (p)
						makePacket(eEnumChannelName, p);
					else
						makePacket(eEnumChannelName, _T(""));
				}
				break;
			}

			case eSetChannel2:
			{
				if (pPh->GetBodyLength() != ((sizeof(DWORD) * 2) + sizeof(BYTE)))
					makePacket(eSetChannel2, (DWORD)0xff);
				else
				{
					m_bChannelLock = pPh->m_pPacket->payload[sizeof(DWORD) * 2];
					BOOL bLocked = FALSE;
					{
#ifndef STRICT_LOCK
						LOCK(g_Lock);
#endif
						for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
						{
							if (*it == this)
								continue;
							if ((m_pIBon != NULL) && (m_pIBon == (*it)->m_pIBon))
							{
								if ((*it)->m_bChannelLock)
									bLocked = TRUE;
								if ((m_hTsRead == NULL) && ((*it)->m_hTsRead != NULL))
								{
									m_hTsRead = (*it)->m_hTsRead;
									m_pTsReceiversList = (*it)->m_pTsReceiversList;
									m_pStopTsRead = (*it)->m_pStopTsRead;
									m_pTsLock = (*it)->m_pTsLock;
									m_ppos = (*it)->m_ppos;
									m_pTsLock->Enter();
									m_pTsReceiversList->push_back(this);
									m_pTsLock->Leave();
								}
							}
						}
					}
					if (bLocked && !m_bChannelLock)
						makePacket(eSetChannel2, (DWORD)0x01);
					else
					{
						if (m_hTsRead)
							m_pTsLock->Enter();
						BOOL b = SetChannel(::ntohl(*(DWORD *)(pPh->m_pPacket->payload)), ::ntohl(*(DWORD *)&(pPh->m_pPacket->payload[sizeof(DWORD)])));
						if (m_hTsRead)
							m_pTsLock->Leave();
						if (b)
						{
							makePacket(eSetChannel2, (DWORD)0x00);
							if (m_hTsRead == NULL)
							{
#ifndef STRICT_LOCK
								// すぐ上で検索してるのになぜ再度検索するのかと言うと、同じBonDriverを要求している複数の
								// クライアントから、ほぼ同時のタイミングで最初のeSetChannel2をリクエストされた場合の為
								// eSetChannel2全体をまとめてロックすれば必要無くなるが、BonDriver_Proxyがロードされ、
								// それが自分自身に接続してきた場合デッドロックする事になる
								// なお、同様の理由でeCreateBonDriver, eOpenTuner, eCloseTunerのロックは実は不完全
								// しかし、自分自身への再帰接続を行わないならば完全なロックも可能
								// 実際の所、テスト用途以外で自分自身への再接続が必要になる状況と言うのはまず無いと
								// 思うので、STRICT_LOCKが定義してある場合は完全なロックを行う事にする
								// ただしそのかわりに、BonDriver_Proxyをロードし、そこからのプロキシチェーンのどこかで
								// 自分自身に再帰接続した場合はデッドロックとなるので注意
								BOOL bFind = FALSE;
								LOCK(g_Lock);
								for (std::list<cProxyServer *>::iterator it = g_InstanceList.begin(); it != g_InstanceList.end(); ++it)
								{
									if (*it == this)
										continue;
									if (m_pIBon == (*it)->m_pIBon)
									{
										if ((*it)->m_hTsRead != NULL)
										{
											bFind = TRUE;
											m_hTsRead = (*it)->m_hTsRead;
											m_pTsReceiversList = (*it)->m_pTsReceiversList;
											m_pStopTsRead = (*it)->m_pStopTsRead;
											m_pTsLock = (*it)->m_pTsLock;
											m_ppos = (*it)->m_ppos;
											m_pTsLock->Enter();
											m_pTsReceiversList->push_back(this);
											m_pTsLock->Leave();
											break;
										}
									}
								}
								if (!bFind)
								{
#endif
									m_pTsReceiversList = new std::list<cProxyServer *>();
									m_pTsReceiversList->push_back(this);
									m_pStopTsRead = new BOOL(FALSE);
									m_pTsLock = new cCriticalSection();
									m_ppos = new DWORD(0);
									LPVOID *ppv = new LPVOID[5];
									ppv[0] = m_pIBon;
									ppv[1] = m_pTsReceiversList;
									ppv[2] = m_pStopTsRead;
									ppv[3] = m_pTsLock;
									ppv[4] = m_ppos;
									m_hTsRead = ::CreateThread(NULL, 0, cProxyServer::TsReader, ppv, 0, NULL);
									if (m_hTsRead == NULL)
									{
										delete[] ppv;
										delete m_pTsReceiversList;
										m_pTsReceiversList = NULL;
										delete m_pStopTsRead;
										m_pStopTsRead = NULL;
										delete m_pTsLock;
										m_pTsLock = NULL;
										delete m_ppos;
										m_ppos = NULL;
										m_Error.Set();
									}
#ifndef STRICT_LOCK
								}
#endif
							}
							else
							{
								LOCK(*m_pTsLock);
								*m_ppos = 0;
							}
						}
						else
							makePacket(eSetChannel2, (DWORD)0xff);
					}
				}
				break;
			}

			case eGetTotalDeviceNum:
				makePacket(eGetTotalDeviceNum, GetTotalDeviceNum());
				break;

			case eGetActiveDeviceNum:
				makePacket(eGetActiveDeviceNum, GetActiveDeviceNum());
				break;

			case eSetLnbPower:
			{
				if (pPh->GetBodyLength() != sizeof(BYTE))
					makePacket(eSetLnbPower, FALSE);
				else
					makePacket(eSetLnbPower, SetLnbPower((BOOL)(pPh->m_pPacket->payload[0])));
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
	::WaitForMultipleObjects(2, hThread, TRUE, INFINITE);
	::CloseHandle(hThread[0]);
	::CloseHandle(hThread[1]);
	return 0;
}

int cProxyServer::ReceiverHelper(char *pDst, DWORD left)
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
		if ((len = ::select(0/*(int)(m_s + 1)*/, &rd, NULL, NULL, &tv)) == SOCKET_ERROR)
		{
			ret = -2;
			goto err;
		}

		if (len == 0)
			continue;

		// MSDNのrecv()のソース例とか見る限り、"SOCKET_ERROR"が負の値なのは保証されてるっぽい
		if ((len = ::recv(m_s, pDst, left, 0)) <= 0)
		{
			ret = -3;
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

DWORD WINAPI cProxyServer::Receiver(LPVOID pv)
{
	cProxyServer *pProxy = static_cast<cProxyServer *>(pv);
	char *p;
	DWORD left, ret;
	cPacketHolder *pPh = NULL;

	for (;;)
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

		left = pPh->GetBodyLength();
		if (left == 0)
		{
			pProxy->m_fifoRecv.Push(pPh);
			continue;
		}

		if (left > 16)
		{
			if (left > 512)
			{
				pProxy->m_Error.Set();
				ret = 203;
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
			ret = 204;
			goto end;
		}

		pProxy->m_fifoRecv.Push(pPh);
	}
end:
	delete pPh;
	return ret;
}

void cProxyServer::makePacket(enumCommand eCmd, BOOL b)
{
	cPacketHolder *p = new cPacketHolder(eCmd, sizeof(BYTE));
	p->m_pPacket->payload[0] = (BYTE)b;
	m_fifoSend.Push(p);
}

void cProxyServer::makePacket(enumCommand eCmd, DWORD dw)
{
	cPacketHolder *p = new cPacketHolder(eCmd, sizeof(DWORD));
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos = ::htonl(dw);
	m_fifoSend.Push(p);
}

void cProxyServer::makePacket(enumCommand eCmd, LPCTSTR str)
{
	register size_t size = (::_tcslen(str) + 1) * sizeof(TCHAR);
	cPacketHolder *p = new cPacketHolder(eCmd, size);
	::memcpy(p->m_pPacket->payload, str, size);
	m_fifoSend.Push(p);
}

void cProxyServer::makePacket(enumCommand eCmd, BYTE *pSrc, DWORD dwSize, float fSignalLevel)
{
	register size_t size = (sizeof(DWORD) * 2) + dwSize;
	cPacketHolder *p = new cPacketHolder(eCmd, size);
	union {
		DWORD dw;
		float f;
	} u;
	u.f = fSignalLevel;
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos++ = ::htonl(dwSize);
	*pos++ = ::htonl(u.dw);
	if (dwSize > 0)
		::memcpy(pos, pSrc, dwSize);
	m_fifoSend.Push(p);
}

DWORD WINAPI cProxyServer::Sender(LPVOID pv)
{
	cProxyServer *pProxy = static_cast<cProxyServer *>(pv);
	DWORD ret;
	HANDLE h[2] = { pProxy->m_Error, pProxy->m_fifoSend.GetEventHandle() };
	for (;;)
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
	return ret;
}

DWORD WINAPI cProxyServer::TsReader(LPVOID pv)
{
	LPVOID *ppv = static_cast<LPVOID *>(pv);
	IBonDriver *pIBon = static_cast<IBonDriver *>(ppv[0]);
	std::list<cProxyServer *> &TsReceiversList = *(static_cast<std::list<cProxyServer *> *>(ppv[1]));
	volatile BOOL &StopTsRead = *(static_cast<BOOL *>(ppv[2]));
	cCriticalSection &TsLock = *(static_cast<cCriticalSection *>(ppv[3]));
	DWORD &pos = *(static_cast<DWORD *>(ppv[4]));
	delete[] ppv;
	DWORD dwSize, dwRemain, now, before = 0;
	float fSignalLevel = 0;
	DWORD ret = 300;
	const DWORD TsPacketBufSize = g_TsPacketBufSize;
	BYTE *pBuf, *pTsBuf = new BYTE[TsPacketBufSize];
#if _DEBUG && DETAILLOG
	DWORD Counter = 0;
#endif

	// TS読み込みループ
	while (!StopTsRead)
	{
		if (((now = ::GetTickCount()) - before) >= 1000)
		{
			TsLock.Enter();
			fSignalLevel = pIBon->GetSignalLevel();
			TsLock.Leave();
			before = now;
		}
		dwSize = dwRemain = 0;
		{
			LOCK(TsLock);
			if (pIBon->GetTsStream(&pBuf, &dwSize, &dwRemain) && (dwSize != 0))
			{
				if ((pos + dwSize) < TsPacketBufSize)
				{
					::memcpy(&pTsBuf[pos], pBuf, dwSize);
					pos += dwSize;
					if (dwRemain == 0)
					{
						for (std::list<cProxyServer *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
							(*it)->makePacket(eGetTsStream, pTsBuf, pos, fSignalLevel);
#if _DEBUG && DETAILLOG
						_RPT3(_CRT_WARN, "makePacket0() : %u : size[%x] / dwRemain[%d]\n", Counter++, pos, dwRemain);
#endif
						pos = 0;
					}
				}
				else
				{
					DWORD left, dwLen = TsPacketBufSize - pos;
					::memcpy(&pTsBuf[pos], pBuf, dwLen);
					for (std::list<cProxyServer *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
						(*it)->makePacket(eGetTsStream, pTsBuf, TsPacketBufSize, fSignalLevel);
#if _DEBUG && DETAILLOG
					_RPT3(_CRT_WARN, "makePacket1() : %u : size[%x] / dwRemain[%d]\n", Counter++, TsPacketBufSize, dwRemain);
#endif
					left = dwSize - dwLen;
					pBuf += dwLen;
					while (left >= TsPacketBufSize)
					{
						for (std::list<cProxyServer *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
							(*it)->makePacket(eGetTsStream, pBuf, TsPacketBufSize, fSignalLevel);
#if _DEBUG && DETAILLOG
						_RPT2(_CRT_WARN, "makePacket2() : %u : size[%x]\n", Counter++, TsPacketBufSize);
#endif
						left -= TsPacketBufSize;
						pBuf += TsPacketBufSize;
					}
					if (left != 0)
					{
						if (dwRemain == 0)
						{
							for (std::list<cProxyServer *>::iterator it = TsReceiversList.begin(); it != TsReceiversList.end(); ++it)
								(*it)->makePacket(eGetTsStream, pBuf, left, fSignalLevel);
#if _DEBUG && DETAILLOG
							_RPT3(_CRT_WARN, "makePacket3() : %u : size[%x] / dwRemain[%d]\n", Counter++, left, dwRemain);
#endif
							left = 0;
						}
						else
							::memcpy(pTsBuf, pBuf, left);
					}
					pos = left;
				}
			}
		}
		if (dwRemain == 0)
			::Sleep(WAIT_TIME);
	}
	delete[] pTsBuf;
	return ret;
}

BOOL cProxyServer::SelectBonDriver(LPCSTR p)
{
	if (p[0] == '\\' && p[1] == '\\')
		return FALSE;
	m_hModule = ::LoadLibraryA(p);
	return (m_hModule != NULL);
}

IBonDriver *cProxyServer::CreateBonDriver()
{
	if (m_hModule)
	{
		IBonDriver *(*f)() = (IBonDriver *(*)())::GetProcAddress(m_hModule, "CreateBonDriver");
		if (f)
		{
			try { m_pIBon = f(); }
			catch (...) {}
			if (m_pIBon)
			{
				m_pIBon2 = dynamic_cast<IBonDriver2 *>(m_pIBon);
				m_pIBon3 = dynamic_cast<IBonDriver3 *>(m_pIBon);
			}
		}
	}
	return m_pIBon;
}

const BOOL cProxyServer::OpenTuner(void)
{
	BOOL b = FALSE;
	if (m_pIBon)
		b = m_pIBon->OpenTuner();
	return b;
}

void cProxyServer::CloseTuner(void)
{
	if (m_pIBon)
		m_pIBon->CloseTuner();
}

void cProxyServer::PurgeTsStream(void)
{
	if (m_pIBon)
		m_pIBon->PurgeTsStream();
}

void cProxyServer::Release(void)
{
	if (m_pIBon)
	{
		if (g_SandBoxedRelease)
		{
			__try { m_pIBon->Release(); }
			__except (EXCEPTION_EXECUTE_HANDLER){}
		}
		else
			m_pIBon->Release();
	}
}

LPCTSTR cProxyServer::EnumTuningSpace(const DWORD dwSpace)
{
	LPCTSTR pStr = NULL;
	if (m_pIBon2)
		pStr = m_pIBon2->EnumTuningSpace(dwSpace);
	return pStr;
}

LPCTSTR cProxyServer::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	LPCTSTR pStr = NULL;
	if (m_pIBon2)
		pStr = m_pIBon2->EnumChannelName(dwSpace, dwChannel);
	return pStr;
}

const BOOL cProxyServer::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	BOOL b = FALSE;
	if (m_pIBon2)
		b = m_pIBon2->SetChannel(dwSpace, dwChannel);
	return b;
}

const DWORD cProxyServer::GetTotalDeviceNum(void)
{
	DWORD d = 0;
	if (m_pIBon3)
		d = m_pIBon3->GetTotalDeviceNum();
	return d;
}

const DWORD cProxyServer::GetActiveDeviceNum(void)
{
	DWORD d = 0;
	if (m_pIBon3)
		d = m_pIBon3->GetActiveDeviceNum();
	return d;
}

const BOOL cProxyServer::SetLnbPower(const BOOL bEnable)
{
	BOOL b = FALSE;
	if (m_pIBon3)
		b = m_pIBon3->SetLnbPower(bEnable);
	return b;
}

#if _DEBUG
struct HostInfo{
	char *host;
	char *port;
};
static DWORD WINAPI Listen(LPVOID pv)
{
	HostInfo *hinfo = static_cast<HostInfo *>(pv);
	char *host = hinfo->host;
	char *port = hinfo->port;
#else
static int Listen(char *host, char *port)
{
#endif
	addrinfo hints, *results, *rp;
	SOCKET lsock, csock;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
	if (getaddrinfo(host, port, &hints, &results) != 0)
	{
		hints.ai_flags = AI_PASSIVE;
		if (getaddrinfo(host, port, &hints, &results) != 0)
			return 1;
	}

	for (rp = results; rp != NULL; rp = rp->ai_next)
	{
		lsock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (lsock == INVALID_SOCKET)
			continue;

		BOOL exclusive = TRUE;
		setsockopt(lsock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof(exclusive));

		if (bind(lsock, rp->ai_addr, (int)(rp->ai_addrlen)) != SOCKET_ERROR)
			break;

		closesocket(lsock);
	}
	freeaddrinfo(results);
	if (rp == NULL)
		return 2;

	if (listen(lsock, 4) == SOCKET_ERROR)
	{
		closesocket(lsock);
		return 3;
	}

	for (;;)
	{
		csock = accept(lsock, NULL, NULL);
		if (csock == INVALID_SOCKET)
			continue;

		cProxyServer *pProxy = new cProxyServer();
		pProxy->setSocket(csock);
		HANDLE hThread = ::CreateThread(NULL, 0, cProxyServer::Reception, pProxy, 0, NULL);
		if (hThread)
			CloseHandle(hThread);
		else
			delete pProxy;
	}

	return 0;	// ここには来ない
}

#if _DEBUG
LRESULT CALLBACK WndProc(HWND hWnd, UINT iMsg, WPARAM wParam, LPARAM lParam)
{
	HDC hDc;
	TCHAR buf[1024];
	static int n = 0;

	switch (iMsg)
	{
	case WM_CREATE:
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_PAINT:
		PAINTSTRUCT ps;
		hDc = BeginPaint(hWnd, &ps);
		wsprintf(buf, TEXT("pProxy [%p] n[%d]"), debug, n);
		TextOut(hDc, 0, 0, buf, (int)_tcslen(buf));
		if (debug)
		{
			wsprintf(buf, TEXT("send_fifo size[%d] recv_fifo size[%d]"), debug->m_fifoSend.Size(), debug->m_fifoRecv.Size());
			TextOut(hDc, 0, 40, buf, (int)_tcslen(buf));
		}
		EndPaint(hWnd, &ps);
		return 0;

	case WM_LBUTTONDOWN:
		n++;
		InvalidateRect(hWnd, NULL, FALSE);
		return 0;
	}
	return DefWindowProc(hWnd, iMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	static HANDLE hLogFile = CreateFile(_T("dbglog.txt"), GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_WARN, hLogFile);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_ERROR, hLogFile);
	_RPT0(_CRT_WARN, "--- PROCESS_START ---\n");
//	int *p = new int[2];	// リーク検出テスト用

	if (Init(hInstance) != 0)
		return -1;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return -2;

	HostInfo hinfo;
	hinfo.host = g_Host;
	hinfo.port = g_Port;
	HANDLE hThread = CreateThread(NULL, 0, Listen, &hinfo, 0, NULL);
	CloseHandle(hThread);

	HWND hWnd;
	MSG msg;
	WNDCLASSEX wndclass;

	wndclass.cbSize = sizeof(wndclass);
	wndclass.style = CS_HREDRAW | CS_VREDRAW;
	wndclass.lpfnWndProc = WndProc;
	wndclass.cbClsExtra = 0;
	wndclass.cbWndExtra = 0;
	wndclass.hInstance = hInstance;
	wndclass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
	wndclass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
	wndclass.lpszMenuName = NULL;
	wndclass.lpszClassName = _T("Debug");
	wndclass.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

	RegisterClassEx(&wndclass);

	hWnd = CreateWindow(_T("Debug"), _T("Debug"), WS_OVERLAPPEDWINDOW, 256, 256, 512, 256, NULL, NULL, hInstance, NULL);

	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);

	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	delete debug;

	WSACleanup();

	_RPT0(_CRT_WARN, "--- PROCESS_END ---\n");
//	CloseHandle(hLogFile);

	return (int)msg.wParam;
}
#else
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE/*hPrevInstance*/, LPSTR/*lpCmdLine*/, int/*nCmdShow*/)
{
	if (Init(hInstance) != 0)
		return -1;

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return -2;

	int ret = Listen(g_Host, g_Port);
	WSACleanup();
	return ret;
}
#endif
