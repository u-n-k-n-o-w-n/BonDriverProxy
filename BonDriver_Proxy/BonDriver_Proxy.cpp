#define _CRT_SECURE_NO_WARNINGS
#include "BonDriver_Proxy.h"
#include <assert.h>

#if _DEBUG
#define DETAILLOG	0
#endif

typedef enum enumWakeupMode {
	MagicPacket = 0,
	PatternMatch,
} WakeupMode;
static BOOL WakeupRemoteComputer(const char* host, WakeupMode mode, UINT repeat = 1);

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
	GetPrivateProfileStringA("OPTION", "BONDRIVER", "BonDriver_ptmr.dll", g_BonDriver, sizeof(g_BonDriver), szIniPath);
	g_ChannelLock = (BYTE)GetPrivateProfileIntA("OPTION", "CHANNEL_LOCK", 0, szIniPath);

	g_ConnectTimeOut = GetPrivateProfileIntA("OPTION", "CONNECT_TIMEOUT", 5, szIniPath);
	g_UseMagicPacket = (UINT)min(GetPrivateProfileIntA("OPTION", "USE_MAGICPACKET", 0, szIniPath), 100);
	g_UsePatternMatch = (UINT)min(GetPrivateProfileIntA("OPTION", "USE_PATTERNMATCH", 0, szIniPath), 100);
	if (g_UseMagicPacket)
	{
		char mac[32];
		GetPrivateProfileStringA("MAGICPACKET", "TARGET_ADDRESS", g_Host, g_TargetHost, sizeof(g_TargetHost), szIniPath);
		GetPrivateProfileStringA("MAGICPACKET", "TARGET_PORT", g_Port, g_TargetPort, sizeof(g_TargetPort), szIniPath);
		memset(mac, 0, sizeof(mac));
		GetPrivateProfileStringA("MAGICPACKET", "TARGET_MACADDRESS", "", mac, sizeof(mac), szIniPath);
		for (int i = 0; i < 6; i++)
		{
			BYTE b = 0;
			p = &mac[i * 3];
			for (int j = 0; j < 2; j++)
			{
				if ('0' <= *p && *p <= '9')
					b = b * 0x10 + (*p - '0');
				else if ('A' <= *p && *p <= 'F')
					b = b * 0x10 + (*p - 'A' + 10);
				else if ('a' <= *p && *p <= 'f')
					b = b * 0x10 + (*p - 'a' + 10);
				else
					return -2;
				p++;
			}
			g_TargetMac[i] = b;
		}
	}

	g_PacketFifoSize = GetPrivateProfileIntA("SYSTEM", "PACKET_FIFO_SIZE", 64, szIniPath);
	g_TsFifoSize = GetPrivateProfileIntA("SYSTEM", "TS_FIFO_SIZE", 64, szIniPath);
	g_TsPacketBufSize = GetPrivateProfileIntA("SYSTEM", "TSPACKET_BUFSIZE", (188 * 1024), szIniPath);

	return 0;
}

cProxyClient::cProxyClient() : m_Error(TRUE, FALSE), m_StartTrigger(TRUE, FALSE)
{
	m_s = INVALID_SOCKET;
	m_LastBuf = NULL;
	m_dwBufPos = 0;
	::memset(m_pBuf, 0, sizeof(m_pBuf));
	m_bBonDriver = m_bTuner = m_bRereased = m_bWaitCNR = FALSE;
	m_fSignalLevel = 0;
	m_dwSpace = m_dwChannel = 0x7fffffff;	// INT_MAX
//	m_hThread = NULL;
	m_lEndCount = -1;
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

	if (m_lEndCount != -1)
		SleepLock(3);

//	if (m_hThread != NULL)
//		::WaitForSingleObject(m_hThread, INFINITE);

	{
		LOCK(m_writeLock);
		for (int i = 0; i < 8; i++)
			delete[] m_pBuf[i];
		TsFlush();
		delete m_LastBuf;
	}

	if (m_s != INVALID_SOCKET)
		::closesocket(m_s);
}

DWORD WINAPI cProxyClient::ProcessEntry(LPVOID pv)
{
	cProxyClient *pProxy = static_cast<cProxyClient *>(pv);
	DWORD ret = pProxy->Process();
	if (ret == 0)
		pProxy->m_lEndCount++;
	else
		pProxy->m_lEndCount = -1;
	return ret;
}

DWORD cProxyClient::Process()
{
	HANDLE hThread[2];
	hThread[0] = ::CreateThread(NULL, 0, cProxyClient::Sender, this, 0, NULL);
	if (hThread[0] == NULL)
	{
		m_Error.Set();
		return 1;
	}

	hThread[1] = ::CreateThread(NULL, 0, cProxyClient::Receiver, this, 0, NULL);
	if (hThread[1] == NULL)
	{
		m_Error.Set();
		::WaitForSingleObject(hThread[0], INFINITE);
		::CloseHandle(hThread[0]);
		return 2;
	}

	m_lEndCount = 0;
	m_StartTrigger.Set();

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
			int idx;
			cPacketHolder *pPh;
			m_fifoRecv.Pop(&pPh);
			switch (pPh->GetCommand())
			{
			case eSelectBonDriver:
				idx = ebResSelectBonDriver;
				goto bres;
			case eCreateBonDriver:
				idx = ebResCreateBonDriver;
				goto bres;
			case eOpenTuner:
				idx = ebResOpenTuner;
				goto bres;
			case ePurgeTsStream:
				idx = ebResPurgeTsStream;
				goto bres;
			case eSetLnbPower:
				idx = ebResSetLnbPower;
			bres:
			{
				LOCK(m_readLock);
				if (pPh->GetBodyLength() != sizeof(BYTE))
					m_bRes[idx] = FALSE;
				else
					m_bRes[idx] = pPh->m_pPacket->payload[0];
				m_bResEvent[idx].Set();
				break;
			}

			case eGetTsStream:
				if (pPh->GetBodyLength() >= (sizeof(DWORD) * 2))
				{
					DWORD dwSize = ::ntohl(*(DWORD *)(pPh->m_pPacket->payload));
					// �ςȃp�P�b�g�͔p��(���K�̃T�[�o�Ɍq���ł���ꍇ�͗��鎖�͂Ȃ��n�Y)
					if ((pPh->GetBodyLength() - (sizeof(DWORD) * 2)) == dwSize)
					{
						union {
							DWORD dw;
							float f;
						} u;
						u.dw = ::ntohl(*(DWORD *)&(pPh->m_pPacket->payload[sizeof(DWORD)]));
						m_fSignalLevel = u.f;
						m_bWaitCNR = FALSE;

						pPh->SetDeleteFlag(FALSE);
						TS_DATA *pData = new TS_DATA();
						pData->dwSize = dwSize;
						pData->pbBufHead = pPh->m_pBuf;
						pData->pbBuf = &(pPh->m_pPacket->payload[sizeof(DWORD) * 2]);
						m_fifoTS.Push(pData);
					}
				}
				break;

			case eEnumTuningSpace:
				idx = epResEnumTuningSpace;
				goto pres;
			case eEnumChannelName:
				idx = epResEnumChannelName;
			pres:
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
					m_pRes[idx] = m_pBuf[m_dwBufPos++];
					m_pResEvent[idx].Set();
				}
				break;
			}

			case eSetChannel2:
				idx = edwResSetChannel2;
				goto dwres;
			case eGetTotalDeviceNum:
				idx = edwResGetTotalDeviceNum;
				goto dwres;
			case eGetActiveDeviceNum:
				idx = edwResGetActiveDeviceNum;
			dwres:
			{
				LOCK(m_readLock);
				if (pPh->GetBodyLength() != sizeof(DWORD))
					m_dwRes[idx] = 0;
				else
					m_dwRes[idx] = ::ntohl(*(DWORD *)(pPh->m_pPacket->payload));
				m_dwResEvent[idx].Set();
				break;
			}

			default:
				break;
			}
			delete pPh;
			break;
		}

		default:
			// �����̃G���[
			m_Error.Set();
			goto end;
		}
	}
end:

//	Sender / Receiver����ExitThread()���Ă΂�Ă�̂Ƀn���h�����V�O�i����ԂɂȂ�Ȃ��B��B
//	���R�m���Ă�l�������狳���ĉ�����(;�L�D`)
//	::WaitForMultipleObjects(2, hThread, TRUE, INFINITE);

	SleepLock(2);	//	���傤���Ȃ��̂ŃX���[�v���b�N�őΉ��c
	::CloseHandle(hThread[0]);
	::CloseHandle(hThread[1]);
	return 0;
}

int cProxyClient::ReceiverHelper(char *pDst, DWORD left)
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

		// MSDN��recv()�̃\�[�X��Ƃ��������A"SOCKET_ERROR"�����̒l�Ȃ͕̂ۏ؂���Ă���ۂ�
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

DWORD WINAPI cProxyClient::Receiver(LPVOID pv)
{
	cProxyClient *pProxy = static_cast<cProxyClient *>(pv);
	char *p;
	DWORD left, ret;
	cPacketHolder *pPh = NULL;
	const DWORD MaxPacketBufSize = g_TsPacketBufSize + (sizeof(DWORD) * 2);

	pProxy->WaitStartTrigger();
	for (;;)
	{
		pPh = new cPacketHolder(MaxPacketBufSize);
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

		if (left > MaxPacketBufSize)
		{
			pProxy->m_Error.Set();
			ret = 203;
			goto end;
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
	::InterlockedIncrement(&(pProxy->m_lEndCount));
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

void cProxyClient::makePacket(enumCommand eCmd, DWORD dw1, DWORD dw2, BYTE b)
{
	cPacketHolder *p = new cPacketHolder(eCmd, (sizeof(DWORD) * 2) + sizeof(BYTE));
	DWORD *pos = (DWORD *)(p->m_pPacket->payload);
	*pos++ = ::htonl(dw1);
	*pos++ = ::htonl(dw2);
	*(BYTE *)pos = b;
	m_fifoSend.Push(p);
}

DWORD WINAPI cProxyClient::Sender(LPVOID pv)
{
	cProxyClient *pProxy = static_cast<cProxyClient *>(pv);
	DWORD ret;
	HANDLE h[2] = { pProxy->m_Error, pProxy->m_fifoSend.GetEventHandle() };

	pProxy->WaitStartTrigger();
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
			// �����̃G���[
			pProxy->m_Error.Set();
			ret = 102;
			goto end;
		}
	}
end:
	::InterlockedIncrement(&(pProxy->m_lEndCount));
	return ret;
}

BOOL cProxyClient::SelectBonDriver()
{
	{
		LOCK(g_Lock);
		makePacket(eSelectBonDriver, g_BonDriver);
	}
	if (m_bResEvent[ebResSelectBonDriver].Wait(m_Error) != WAIT_OBJECT_0)
	{
		LOCK(m_readLock);
		return m_bRes[ebResSelectBonDriver];
	}
	return FALSE;
}

BOOL cProxyClient::CreateBonDriver()
{
	makePacket(eCreateBonDriver);
	if (m_bResEvent[ebResCreateBonDriver].Wait(m_Error) != WAIT_OBJECT_0)
	{
		LOCK(m_readLock);
		if (m_bRes[ebResCreateBonDriver])
			m_bBonDriver = TRUE;
		return m_bRes[ebResCreateBonDriver];
	}
	return FALSE;
}

const BOOL cProxyClient::OpenTuner(void)
{
	if (!m_bBonDriver)
		return FALSE;
	makePacket(eOpenTuner);
	if (m_bResEvent[ebResOpenTuner].Wait(m_Error) != WAIT_OBJECT_0)
	{
		LOCK(m_readLock);
		if (m_bRes[ebResOpenTuner])
			m_bTuner = TRUE;
		return m_bRes[ebResOpenTuner];
	}
	return FALSE;
}

void cProxyClient::CloseTuner(void)
{
	if (!m_bTuner)
		return;

	makePacket(eCloseTuner);
	m_bTuner = m_bWaitCNR = FALSE;
	m_fSignalLevel = 0;
	m_dwSpace = m_dwChannel = 0x7fffffff;	// INT_MAX
	{
		LOCK(m_writeLock);
		m_dwBufPos = 0;
		for (int i = 0; i < 8; i++)
			delete[] m_pBuf[i];
		::memset(m_pBuf, 0, sizeof(m_pBuf));
	}
}

const BOOL cProxyClient::SetChannel(const BYTE/*bCh*/)
{
	return TRUE;
}

const float cProxyClient::GetSignalLevel(void)
{
	// �C�x���g�ł�낤���Ǝv�������ǁA�f�b�h���b�N��h���ׂɂ͔������鏈���̉񐔂̓��唼�ł�
	// �S�����ʂȃC�x���g�������������鎖�ɂȂ�̂ŁA�|�[�����O�ł�鎖�ɂ���
	// �Ȃ��A�▭�ȃ^�C�~���O�ł̃l�b�g���[�N�ؒf���ɔ����āA�ő�ł���0.5�b�܂ł����҂��Ȃ�
	for (int i = 0; m_bWaitCNR && (i < 50); i++)
		::Sleep(10);
	return m_fSignalLevel;
}

const DWORD cProxyClient::WaitTsStream(const DWORD dwTimeOut)
{
	if (!m_bTuner)
		return WAIT_ABANDONED;

	HANDLE h[2] = { m_Error, m_fifoTS.GetEventHandle() };
	DWORD ret = ::WaitForMultipleObjects(2, h, FALSE, (dwTimeOut) ? dwTimeOut : INFINITE);
	switch (ret)
	{
	case WAIT_ABANDONED:
	case WAIT_OBJECT_0:
		return WAIT_ABANDONED;

	case WAIT_OBJECT_0 + 1:
		ret = WAIT_OBJECT_0;
	case WAIT_TIMEOUT:	// fall-through
		return ret;

	default:
		return WAIT_FAILED;
	}
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
			delete m_LastBuf;
			m_fifoTS.Pop(&m_LastBuf);
			*ppDst = m_LastBuf->pbBuf;
			*pdwSize = m_LastBuf->dwSize;
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
	if (m_bResEvent[ebResPurgeTsStream].Wait(m_Error) != WAIT_OBJECT_0)
	{
		BOOL b;
		{
			LOCK(m_readLock);
			b = m_bRes[ebResPurgeTsStream];
		}
		if (b)
		{
			LOCK(m_writeLock);
			TsFlush();
		}
	}
}

void cProxyClient::Release(void)
{
	if (m_bTuner)
		CloseTuner();
	makePacket(eRelease);
	m_bRereased = TRUE;
	{
		LOCK(g_Lock);
		g_InstanceList.remove(this);
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
	if (m_pResEvent[epResEnumTuningSpace].Wait(m_Error) != WAIT_OBJECT_0)
	{
		LOCK(m_readLock);
		return m_pRes[epResEnumTuningSpace];
	}
	return NULL;
}

LPCTSTR cProxyClient::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	if (!m_bTuner)
		return NULL;
	makePacket(eEnumChannelName, dwSpace, dwChannel);
	if (m_pResEvent[epResEnumChannelName].Wait(m_Error) != WAIT_OBJECT_0)
	{
		LOCK(m_readLock);
		return m_pRes[epResEnumChannelName];
	}
	return NULL;
}

const BOOL cProxyClient::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	if (!m_bTuner)
		goto err;
//	if ((m_dwSpace == dwSpace) && (m_dwChannel == dwChannel))
//		return TRUE;
	makePacket(eSetChannel2, dwSpace, dwChannel, g_ChannelLock);
	DWORD dw;
	if (m_dwResEvent[edwResSetChannel2].Wait(m_Error) != WAIT_OBJECT_0)
	{
		LOCK(m_readLock);
		dw = m_dwRes[edwResSetChannel2];
	}
	else
		dw = 0xff;
	switch (dw)
	{
	case 0x00:	// ����
	{
		LOCK(m_writeLock);
		TsFlush();
		m_dwSpace = dwSpace;
		m_dwChannel = dwChannel;
		m_fSignalLevel = 0;
		m_bWaitCNR = TRUE;
	}
	case 0x01:	// fall-through / �`�����l�����b�N����Ă�
		return TRUE;
	default:
		break;
	}
err:
	m_fSignalLevel = 0;
	return FALSE;
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
	if (m_dwResEvent[edwResGetTotalDeviceNum].Wait(m_Error) != WAIT_OBJECT_0)
	{
		LOCK(m_readLock);
		return m_dwRes[edwResGetTotalDeviceNum];
	}
	return 0;
}

const DWORD cProxyClient::GetActiveDeviceNum(void)
{
	if (!m_bTuner)
		return 0;
	makePacket(eGetActiveDeviceNum);
	if (m_dwResEvent[edwResGetActiveDeviceNum].Wait(m_Error) != WAIT_OBJECT_0)
	{
		LOCK(m_readLock);
		return m_dwRes[edwResGetActiveDeviceNum];
	}
	return 0;
}

const BOOL cProxyClient::SetLnbPower(const BOOL bEnable)
{
	if (!m_bTuner)
		return FALSE;
	makePacket(eSetLnbPower, bEnable);
	if (m_bResEvent[ebResSetLnbPower].Wait(m_Error) != WAIT_OBJECT_0)
	{
		LOCK(m_readLock);
		return m_bRes[ebResSetLnbPower];
	}
	return FALSE;
}

static SOCKET Connect(char *host, char *port)
{
	addrinfo hints, *results, *rp;
	SOCKET sock;
	int i;
	unsigned long bf;
	fd_set wd;
	timeval tv;

	memset(&hints, 0, sizeof(hints));
	if (g_UseMagicPacket)
		WakeupRemoteComputer(g_TargetHost, MagicPacket, g_UseMagicPacket);
	if (g_UsePatternMatch)
		WakeupRemoteComputer(host, PatternMatch, g_UsePatternMatch);

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_NUMERICHOST;
	if (getaddrinfo(host, port, &hints, &results) != 0)
	{
		hints.ai_flags = 0;
		if (getaddrinfo(host, port, &hints, &results) != 0)
			return INVALID_SOCKET;
	}

	for (rp = results; rp != NULL; rp = rp->ai_next)
	{
		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sock == INVALID_SOCKET)
			continue;

		bf = TRUE;
		ioctlsocket(sock, FIONBIO, &bf);
		tv.tv_sec = g_ConnectTimeOut;
		tv.tv_usec = 0;
		FD_ZERO(&wd);
		FD_SET(sock, &wd);
		connect(sock, rp->ai_addr, (int)(rp->ai_addrlen));
		if ((i = select(0/*(int)(sock + 1)*/, 0, &wd, 0, &tv)) != SOCKET_ERROR)
		{
			// �^�C���A�E�g���Ԃ�"�S�̂�"�ł͂Ȃ�"�X�̃\�P�b�g��"�ɂȂ邯�ǁA�Ƃ肠���������
			if (i != 0)
			{
				bf = FALSE;
				ioctlsocket(sock, FIONBIO, &bf);
				break;
			}
		}
		closesocket(sock);
	}
	freeaddrinfo(results);
	if (rp == NULL)
		return INVALID_SOCKET;

	return sock;
}

extern "C" __declspec(dllexport) IBonDriver *CreateBonDriver()
{
	{
		LOCK(g_Lock);
		if (g_bWinSockInit)
		{
			WSADATA wsa;
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
				return NULL;
			g_bWinSockInit = FALSE;
		}
	}

	SOCKET s = Connect(g_Host, g_Port);
	if (s == INVALID_SOCKET)
		return NULL;

	cProxyClient *pProxy = new cProxyClient();
	pProxy->setSocket(s);
	HANDLE hThread = ::CreateThread(NULL, 0, cProxyClient::ProcessEntry, pProxy, 0, NULL);
	if (hThread == NULL)
		goto err;
//	pProxy->setThreadHandle(hThread);

	if (pProxy->WaitStartTrigger() == WAIT_OBJECT_0)
		goto err;

	if (!pProxy->SelectBonDriver())
		goto err;

	if (pProxy->CreateBonDriver())
	{
		LOCK(g_Lock);
		g_InstanceList.push_back(pProxy);
		return pProxy;
	}

err:
	delete pProxy;
	return NULL;
}

extern "C" __declspec(dllexport) BOOL SetBonDriver(LPCSTR p)
{
	LOCK(g_Lock);
	if (strlen(p) >= sizeof(g_BonDriver))
		return FALSE;
	strcpy(g_BonDriver, p);
	return TRUE;
}

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID/*lpvReserved*/)
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
		break;
	}

	case DLL_PROCESS_DETACH:
	{
		{
			LOCK(g_Lock);
			while (!g_InstanceList.empty())
			{
				cProxyClient *pProxy = g_InstanceList.front();
				g_InstanceList.pop_front();
				delete pProxy;
			}
			if (!g_bWinSockInit)
				WSACleanup();
		}
#if _DEBUG
		_RPT0(_CRT_WARN, "--- DLL_PROCESS_DETACH ---\n");
//		CloseHandle(hLogFile);
#endif
		break;
	}
	}
	return TRUE;
}

static BOOL WakeupRemoteComputer(const char* host, WakeupMode mode, UINT repeat/*=1*/)
{
	addrinfo hints, *results, *rp;
	SOCKET sock;
	int i;
	BOOL result = FALSE;
	unsigned long bf;

	rp = NULL;
	memset(&hints, 0, sizeof(hints));
	try
	{
		switch (mode)
		{
			case MagicPacket:
				char sendbuf[128];
				memset(sendbuf, 0xff, 6);
				for (i = 1; i <= 16; i++)
					memcpy(&sendbuf[i * 6], g_TargetMac, 6);

				hints.ai_family = AF_INET;
				hints.ai_socktype = SOCK_DGRAM;
				hints.ai_protocol = IPPROTO_UDP;
				hints.ai_flags = AI_V4MAPPED | AI_NUMERICHOST;
				if (getaddrinfo(g_TargetHost, g_TargetPort, &hints, &results) != 0)
				{
					hints.ai_flags = AI_V4MAPPED;
					if (getaddrinfo(g_TargetHost, g_TargetPort, &hints, &results) != 0)
						throw __LINE__;
				}

				for (UINT j = 1; j <= repeat; j++)
				{
					for (rp = results; rp != NULL; rp = rp->ai_next)
					{
						sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
						if (sock == INVALID_SOCKET)
							continue;

						BOOL opt = TRUE;
						if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&opt, sizeof(opt)) != 0)
						{
							closesocket(sock);
							continue;
						}

						int ret = sendto(sock, sendbuf, 102, 0, rp->ai_addr, (int)(rp->ai_addrlen));
						closesocket(sock);
						if (ret == 102)
							break;
					}
					if (j < repeat)
						::Sleep(10);
				}
				break;

			case PatternMatch:
				hints.ai_family = AF_UNSPEC;
				hints.ai_socktype = SOCK_STREAM;
				hints.ai_protocol = IPPROTO_TCP;
				hints.ai_flags = AI_NUMERICHOST;

				sock = INVALID_SOCKET;
				if (getaddrinfo(host, "445", &hints, &results) != 0)
				{
					hints.ai_flags = 0;
					if (getaddrinfo(host, "445", &hints, &results) != 0)
						throw __LINE__;
				}

				for (UINT j = 1; j <= repeat; j++)
				{
					for (rp = results; rp != NULL; rp = rp->ai_next)
					{
						sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
						if (sock == INVALID_SOCKET)
							continue;
						if (connect(sock, rp->ai_addr, (int)(rp->ai_addrlen)) == SOCKET_ERROR)
							continue;
						if (shutdown(sock, SD_BOTH) == SOCKET_ERROR)
							continue;
						if (sock != INVALID_SOCKET)
							closesocket(sock);
					}
					if (j < repeat)
						::Sleep(10);
				}
				break;

			default:
				assert(FALSE);
				throw __LINE__;
		}

		freeaddrinfo(results);
		if (rp == NULL)
			throw __LINE__;

		result = TRUE;
	}
	catch (const int &e) {
		e;
#if _DEBUG
#endif
		result = FALSE;
	}
	return result;
}

