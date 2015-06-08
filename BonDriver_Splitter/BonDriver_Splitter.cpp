#define _CRT_SECURE_NO_WARNINGS
#include "BonDriver_Splitter.h"

static void InitCrc32Table()
{
	DWORD i, j, crc;
	for (i = 0; i < 256; i++)
	{
		crc = i << 24;
		for (j = 0; j < 8; j++)
			crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
		g_Crc32Table[i] = crc;
	}
}

static DWORD CalcCRC32(BYTE *p, DWORD len)
{
	DWORD i, crc = 0xffffffff;
	for (i = 0; i < len; i++)
		crc = (crc << 8) ^ g_Crc32Table[(crc >> 24) ^ p[i]];
	return crc;
}

static int Init(HMODULE hModule)
{
	char szIniPath[MAX_PATH + 16] = {};
	GetModuleFileNameA(hModule, szIniPath, MAX_PATH);
	char *p = strrchr(szIniPath, '.');
	if (!p)
		return -1;
	p++;
	strcpy(p, "ini");

	char dbgbuf[1024];
	HANDLE hFile = CreateFileA(szIniPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
	{
		wsprintfA(dbgbuf, "ini file not found: [%s]\n", szIniPath);
		OutputDebugStringA(dbgbuf);
		return -2;
	}
	CloseHandle(hFile);

	g_UseServiceID = GetPrivateProfileIntA("OPTION", "USESERVICEID", 1, szIniPath);
	g_ModPMT = GetPrivateProfileIntA("OPTION", "MODPMT", 0, szIniPath);
	g_TsSync = GetPrivateProfileIntA("OPTION", "TSSYNC", 0, szIniPath);
	g_dwDelFlag = 0;
	char buf[512];
	GetPrivateProfileStringA("OPTION", "DEL", "", buf, sizeof(buf), szIniPath);
	if (buf[0] != '\0')
	{
		const char *name[] = { "EIT", "H-EIT", "M-EIT", "L-EIT", "CAT", "NIT", "SDT", "TOT", "SDTT", "BIT", "CDT", "ECM", "EMM", "TYPED", NULL };
		p = buf;
		int n, cnt = 1;
		while (*p != '\0')
		{
			if (*p == ',')
				cnt++;
			p++;
		}
		char **pp = new char *[cnt];
		p = buf;
		n = 0;
		do
		{
			while (*p == '\t' || *p == ' ')
				p++;
			pp[n++] = p;
			while (*p != '\t' && *p != ' ' && *p != ',' && *p != '\0')
				p++;
			if (*p != ',' && *p != '\0')
			{
				*p++ = '\0';
				while (*p != ',' && *p != '\0')
					p++;
			}
			*p++ = '\0';
		} while (n < cnt);
		for (int i = 0; i < cnt; i++)
		{
			for (int j = 0; name[j] != NULL; j++)
			{
				if (strcmp(pp[i], name[j]) == 0)
				{
					if (j == 0)
						g_dwDelFlag |= 0x7;		// EIT = H-EIT | M-EIT | L-EIT
					else
						g_dwDelFlag |= (1 << (j - 1));
					break;
				}
			}
		}
		delete[] pp;
	}

	g_TsFifoSize = GetPrivateProfileIntA("SYSTEM", "TS_FIFO_SIZE", 128, szIniPath);
	g_TsPacketBufSize = GetPrivateProfileIntA("SYSTEM", "TSPACKET_BUFSIZE", (188 * 256), szIniPath);

	char szPath[sizeof(szIniPath) + sizeof(buf)];
	p = strrchr(szIniPath, '\\');
	if (!p)
		return -3;
	p++;
	strncpy(szPath, szIniPath, p - szIniPath);
	szPath[p - szIniPath] = '\0';
	p = &szPath[p - szIniPath];

	char key[4];
	key[2] = '\0';
	for (int i = 0; i < MAX_DRIVER; i++)
	{
		key[0] = (char)('0' + (i / 10));
		key[1] = (char)('0' + (i % 10));
		GetPrivateProfileStringA("BONDRIVER", key, "", buf, sizeof(buf), szIniPath);
		if (buf[0] == '\0')
			break;
		if (((buf[0] >= 'A' && buf[0] <= 'Z') || (buf[0] >= 'a' && buf[0] <= 'z')) && buf[1] == ':' && buf[2] == '\\')
			g_vBonDrivers.push_back(buf);
		else
		{
			strcpy(p, buf);
			g_vBonDrivers.push_back(szPath);
		}
	}

	for (size_t i = 0; i < g_vBonDrivers.size(); i++)
	{
		hFile = CreateFileA(g_vBonDrivers[i].c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE)
		{
			wsprintfA(dbgbuf, "BonDriver not found: [%s]\n", g_vBonDrivers[i].c_str());
			OutputDebugStringA(dbgbuf);
			return -4;
		}
		CloseHandle(hFile);
	}

	char section[8];
	strcpy(section, "SPACE");
	section[7] = '\0';
	key[3] = '\0';
	for (int i = 0; i < MAX_SPACE; i++)
	{
		section[5] = (char)('0' + (i / 10));
		section[6] = (char)('0' + (i % 10));
		GetPrivateProfileStringA(section, "NAME", "", buf, sizeof(buf), szIniPath);
		if (buf[0] == '\0')
			break;
		stSpace s;
		if (MultiByteToWideChar(CP_UTF8, 0, buf, -1, s.SpaceName, _countof(s.SpaceName)) == 0)
		{
			wsprintfA(dbgbuf, "MultiByteToWideChar() error(Space name): [%s]\n", buf);
			OutputDebugStringA(dbgbuf);
			return -5;
		}
		for (int j = 0; j < MAX_CH; j++)
		{
			key[0] = (char)('0' + (j / 100));
			key[1] = (char)('0' + ((j % 100) / 10));
			key[2] = (char)('0' + (j % 10));
			GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), szIniPath);
			if (buf[0] == '\0')
				break;
			int n = 0;
			char *cp[5];
			BOOL bOk = FALSE;
			p = cp[n++] = buf;
			for (;;)
			{
				p = strchr(p, '\t');
				if (p)
				{
					*p++ = '\0';
					cp[n++] = p;
					if (g_UseServiceID)
					{
						if (n > 4)
						{
							bOk = TRUE;
							break;
						}
					}
					else
					{
						if (n > 3)
						{
							bOk = TRUE;
							break;
						}
					}
				}
				else
					break;
			}
			if (bOk == FALSE)
			{
				GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), szIniPath);
				wsprintfA(dbgbuf, "setting error(column num): line[%s]\n", buf);
				OutputDebugStringA(dbgbuf);
				return -6;
			}
			DWORD dw = strtoul(cp[1], NULL, 0);
			if (dw >= g_vBonDrivers.size())
			{
				GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), szIniPath);
				wsprintfA(dbgbuf, "setting error(BonDriverNo): line[%s]\n", buf);
				OutputDebugStringA(dbgbuf);
				return -7;
			}
			stChannel c;
			if (MultiByteToWideChar(CP_UTF8, 0, cp[0], -1, c.ChName, _countof(c.ChName)) == 0)
			{
				wsprintfA(dbgbuf, "MultiByteToWideChar() error(Channel name): [%s]\n", cp[0]);
				OutputDebugStringA(dbgbuf);
				return -8;
			}
			c.BonNo = (int)dw;
			c.BonSpace = strtoul(cp[2], NULL, 0);
			c.BonChannel = strtoul(cp[3], NULL, 0);
			if (g_UseServiceID)
				c.ServiceID = strtoul(cp[4], NULL, 0);
			else
				c.ServiceID = 0;
			s.vstChannel.push_back(c);
		}
		g_vstSpace.push_back(s);
	}

	if (g_UseServiceID)
		InitCrc32Table();

#ifdef _DEBUG
	for (size_t i = 0; i < g_vBonDrivers.size(); i++)
	{
		_RPT2(_CRT_WARN, "[%02d][%s]\n", i, g_vBonDrivers[i].c_str());
	}
	_RPT0(_CRT_WARN, "-----\n");
	for (size_t i = 0; i < g_vstSpace.size(); i++)
	{
		wsprintfA(buf, "[SPACE%02d][%ls]\n", i, g_vstSpace[i].SpaceName);
		_RPT1(_CRT_WARN, "%s", buf);
		for (size_t j = 0; j < g_vstSpace[i].vstChannel.size(); j++)
		{
			wsprintfA(buf, "[%03d][%ls][%d][%u][%u][%u]\n",
				j,
				g_vstSpace[i].vstChannel[j].ChName,
				g_vstSpace[i].vstChannel[j].BonNo,
				g_vstSpace[i].vstChannel[j].BonSpace,
				g_vstSpace[i].vstChannel[j].BonChannel,
				g_vstSpace[i].vstChannel[j].ServiceID);
			_RPT1(_CRT_WARN, "%s", buf);
		}
		_RPT0(_CRT_WARN, "-----\n");
	}
	wsprintfA(buf, "g_TsFifoSize[%u] g_TsPacketBufSize[%u]\ng_ModPMT[%s] g_TsSync[%s] g_dwDelFlag[0x%x]\n",
		(DWORD)g_TsFifoSize,
		g_TsPacketBufSize,
		g_ModPMT ? "TRUE" : "FALSE",
		g_TsSync ? "TRUE" : "FALSE",
		g_dwDelFlag);
	_RPT1(_CRT_WARN, "%s", buf);
#endif

	return 0;
}

cBonDriverSplitter::cBonDriverSplitter() : m_eCloseTuner(TRUE, TRUE), m_StopTsSplit(TRUE, FALSE)
{
	m_spThis = this;
	m_hBonModule = NULL;
	m_pIBon2 = NULL;
	m_LastBuf = NULL;
	m_bTuner = FALSE;
	m_iBonNo = -1;
	m_dwSpace = m_dwChannel = 0x7fffffff;	// INT_MAX
	m_dwServiceID = 0xffffffff;
	m_hTsRead = m_hTsSplit = NULL;
	m_bStopTsRead = m_bChannelChanged = FALSE;
}

cBonDriverSplitter::~cBonDriverSplitter()
{
	m_bonLock.Enter();
	if (m_hBonModule != NULL)
	{
		CloseTuner();
		if (m_pIBon2 != NULL)
			m_pIBon2->Release();
		::FreeLibrary(m_hBonModule);
	}
	m_bonLock.Leave();

	m_writeLock.Enter();
	TsFlush(g_UseServiceID);
	delete m_LastBuf;
	m_writeLock.Leave();

	m_spThis = NULL;
}

const BOOL cBonDriverSplitter::OpenTuner(void)
{
	if (m_bTuner)
		return TRUE;
	m_eCloseTuner.Reset();
	m_bTuner = TRUE;
	return TRUE;
}

void cBonDriverSplitter::CloseTuner(void)
{
	if (m_bTuner)
	{
		m_bonLock.Enter();
		if (m_pIBon2 != NULL)
		{
			if (m_hTsRead != NULL)
			{
				m_bStopTsRead = TRUE;
				::WaitForSingleObject(m_hTsRead, INFINITE);
				::CloseHandle(m_hTsRead);
				m_hTsRead = NULL;
			}
			m_pIBon2->CloseTuner();
		}
		m_bonLock.Leave();
		m_eCloseTuner.Set();
		m_bTuner = FALSE;
	}
}

const BOOL cBonDriverSplitter::SetChannel(const BYTE/*bCh*/)
{
	return FALSE;
}

const float cBonDriverSplitter::GetSignalLevel(void)
{
	float f;
	m_bonLock.Enter();
	if (m_bTuner && m_pIBon2 != NULL)
		f = m_pIBon2->GetSignalLevel();
	else
		f = 0;
	m_bonLock.Leave();
	return f;
}

const DWORD cBonDriverSplitter::WaitTsStream(const DWORD dwTimeOut)
{
	if (!m_bTuner)
		return WAIT_ABANDONED;
	HANDLE h[2] = { m_eCloseTuner, m_fifoTS.GetEventHandle() };
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

const DWORD cBonDriverSplitter::GetReadyCount(void)
{
	if (!m_bTuner)
		return 0;
	return (DWORD)m_fifoTS.Size();
}

const BOOL cBonDriverSplitter::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
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

const BOOL cBonDriverSplitter::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!m_bTuner)
		return FALSE;
	BOOL b;
	m_writeLock.Enter();
	if (m_fifoTS.Size() != 0)
	{
		delete m_LastBuf;
		m_fifoTS.Pop(&m_LastBuf);
		*ppDst = m_LastBuf->pbBuf;
		*pdwSize = m_LastBuf->dwSize;
		*pdwRemain = (DWORD)m_fifoTS.Size();
		b = TRUE;
	}
	else
	{
		*pdwSize = 0;
		*pdwRemain = 0;
		b = FALSE;
	}
	m_writeLock.Leave();
	return b;
}

void cBonDriverSplitter::PurgeTsStream(void)
{
	if (!m_bTuner)
		return;
	m_bonLock.Enter();
	if (m_pIBon2 != NULL)
		m_pIBon2->PurgeTsStream();
	m_bonLock.Leave();
	m_writeLock.Enter();
	TsFlush(g_UseServiceID);
	m_writeLock.Leave();
}

void cBonDriverSplitter::Release(void)
{
	m_sInstanceLock.Enter();
	delete this;
	m_sInstanceLock.Leave();
}

LPCTSTR cBonDriverSplitter::GetTunerName(void)
{
	return TUNER_NAME;
}

const BOOL cBonDriverSplitter::IsTunerOpening(void)
{
	return FALSE;
}

LPCTSTR cBonDriverSplitter::EnumTuningSpace(const DWORD dwSpace)
{
	if (!m_bTuner)
		return NULL;
	if (dwSpace >= g_vstSpace.size())
		return NULL;
	return g_vstSpace[dwSpace].SpaceName;
}

LPCTSTR cBonDriverSplitter::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	if (!m_bTuner)
		return NULL;
	if (dwSpace >= g_vstSpace.size())
		return NULL;
	if (dwChannel >= g_vstSpace[dwSpace].vstChannel.size())
		return NULL;
	return g_vstSpace[dwSpace].vstChannel[dwChannel].ChName;
}

const BOOL cBonDriverSplitter::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	BOOL bLoad, bChange;
#ifdef _DEBUG
	char buf[1024];
	int n;
	n = ::wsprintfA(buf, "--- Request SetChannel(%u, %u) ---\n", dwSpace, dwChannel);
#else
	char dbgbuf[1024];
#endif
	if (!m_bTuner)
	{
#ifdef _DEBUG
		n += ::wsprintfA(&buf[n], "    -> [FALSE] : Tuner unopened\n");
#endif
		goto err1;
	}
	if (dwSpace >= g_vstSpace.size())
	{
#ifdef _DEBUG
		n += ::wsprintfA(&buf[n], "    -> [FALSE] : dwSpace[%u] >= g_vstSpace.size()[%u]\n", dwSpace, (DWORD)g_vstSpace.size());
#endif
		goto err1;
	}
	if (dwChannel >= g_vstSpace[dwSpace].vstChannel.size())
	{
#ifdef _DEBUG
		n += ::wsprintfA(&buf[n], "    -> [FALSE] : dwChannel[%u] >= vstChannel.size()[%u]\n", dwChannel, (DWORD)g_vstSpace[dwSpace].vstChannel.size());
#endif
		goto err1;
	}

	DWORD dwOldServiceID;
	{
		LOCK(m_bonLock);
		bLoad = FALSE;
		if (g_vstSpace[dwSpace].vstChannel[dwChannel].BonNo != m_iBonNo)
			bLoad = TRUE;

		dwOldServiceID = m_dwServiceID;
		bChange = TRUE;
		if (g_UseServiceID)
		{
			if (!bLoad && m_dwSpace != 0x7fffffff/*&& m_dwChannel != 0x7fffffff*/)
			{
				if ((g_vstSpace[dwSpace].vstChannel[dwChannel].BonSpace == g_vstSpace[m_dwSpace].vstChannel[m_dwChannel].BonSpace) &&
					(g_vstSpace[dwSpace].vstChannel[dwChannel].BonChannel == g_vstSpace[m_dwSpace].vstChannel[m_dwChannel].BonChannel))
				{
					bChange = FALSE;
				}
			}
			m_bChannelChanged = TRUE;
			m_dwServiceID = g_vstSpace[dwSpace].vstChannel[dwChannel].ServiceID;
		}

#ifdef _DEBUG
		n += ::wsprintfA(&buf[n], "    bChange[%s] bLoad[%s] BonNo[%d] BonSpace[%u] BonChannel[%u]\n",
			bChange ? "TRUE" : "FALSE",
			bLoad ? "TRUE" : "FALSE",
			g_vstSpace[dwSpace].vstChannel[dwChannel].BonNo,
			g_vstSpace[dwSpace].vstChannel[dwChannel].BonSpace,
			g_vstSpace[dwSpace].vstChannel[dwChannel].BonChannel);
		_RPT1(_CRT_WARN, "%s", buf);
		n = 0;
#endif

		if (bChange)
		{
			if (bLoad)
			{
				if (m_hBonModule != NULL)
				{
					if (m_pIBon2 != NULL)
					{
						if (m_hTsRead != NULL)
						{
							m_bStopTsRead = TRUE;
							::WaitForSingleObject(m_hTsRead, INFINITE);
							::CloseHandle(m_hTsRead);
							m_hTsRead = NULL;
						}
						m_pIBon2->CloseTuner();
						m_pIBon2->Release();
						m_pIBon2 = NULL;
					}
					::FreeLibrary(m_hBonModule);
					m_hBonModule = NULL;
					m_iBonNo = -1;
				}

				int iBonNo = g_vstSpace[dwSpace].vstChannel[dwChannel].BonNo;
				HMODULE hModule = ::LoadLibraryA(g_vBonDrivers[iBonNo].c_str());
				if (hModule == NULL)
				{
#ifdef _DEBUG
					n += ::wsprintfA(&buf[n], "    -> [FALSE] : LoadLibrary(\"%s\") error\n", g_vBonDrivers[iBonNo].c_str());
#else
					::wsprintfA(dbgbuf, "*** LoadLibrary(\"%s\") error ***\n", g_vBonDrivers[iBonNo].c_str());
					::OutputDebugStringA(dbgbuf);
#endif
					goto err0;
				}
				IBonDriver *pIBon = NULL;
				IBonDriver2 *pIBon2 = NULL;
				IBonDriver *(*f)() = (IBonDriver *(*)())::GetProcAddress(hModule, "CreateBonDriver");
				if (f)
				{
					pIBon = f();
					if (pIBon)
						pIBon2 = dynamic_cast<IBonDriver2 *>(pIBon);
				}
				if (pIBon2 == NULL)
				{
#ifdef _DEBUG
					n += ::wsprintfA(&buf[n], "    -> [FALSE] : CreateBonDriver() error / pIBon[%p] pIBon2[%p]\n", pIBon, pIBon2);
#else
					::wsprintfA(dbgbuf, "*** CreateBonDriver() error / pIBon[%p] pIBon2[%p] ***\n", pIBon, pIBon2);
					::OutputDebugStringA(dbgbuf);
#endif
					if (pIBon)
						pIBon->Release();
					::FreeLibrary(hModule);
					goto err0;
				}
				if (pIBon->OpenTuner() == FALSE)
				{
#ifdef _DEBUG
					n += ::wsprintfA(&buf[n], "    -> [FALSE] : OpenTuner() error\n");
#else
					::wsprintfA(dbgbuf, "*** pIBon->OpenTuner() error ***\n");
					::OutputDebugStringA(dbgbuf);
#endif
					pIBon->Release();
					::FreeLibrary(hModule);
					goto err0;
				}
				m_iBonNo = iBonNo;
				m_hBonModule = hModule;
				m_pIBon2 = pIBon2;
				::Sleep(10);	// for Windows PT1/2 SDK
			}
			if (m_pIBon2->SetChannel(g_vstSpace[dwSpace].vstChannel[dwChannel].BonSpace, g_vstSpace[dwSpace].vstChannel[dwChannel].BonChannel) == FALSE)
			{
#ifdef _DEBUG
				n += ::wsprintfA(&buf[n], "    -> [FALSE] : m_pIBon2->SetChannel(%u, %u) error\n", g_vstSpace[dwSpace].vstChannel[dwChannel].BonSpace, g_vstSpace[dwSpace].vstChannel[dwChannel].BonChannel);
#else
				::wsprintfA(dbgbuf, "*** m_pIBon2->SetChannel(%u, %u) error ***\n", g_vstSpace[dwSpace].vstChannel[dwChannel].BonSpace, g_vstSpace[dwSpace].vstChannel[dwChannel].BonChannel);
				::OutputDebugStringA(dbgbuf);
#endif
				goto err0;
			}
		}
	}

	m_writeLock.Enter();
	TsFlush(g_UseServiceID);
	m_writeLock.Leave();

	if (m_hTsRead == NULL)
	{
		m_bStopTsRead = FALSE;
		m_hTsRead = ::CreateThread(NULL, 0, TsReader, this, 0, NULL);
		if (m_hTsRead == NULL)
		{
#ifdef _DEBUG
			n += ::wsprintfA(&buf[n], "    -> [FALSE] : CreateThread() error\n");
#endif
			goto err0;
		}
	}
	m_dwSpace = dwSpace;
	m_dwChannel = dwChannel;
#ifdef _DEBUG
	n += ::wsprintfA(&buf[n], "    -> [TRUE] : ok\n");
	_RPT1(_CRT_WARN, "%s", buf);
#endif
	return TRUE;
err0:
	m_bChannelChanged = FALSE;
	m_dwServiceID = dwOldServiceID;
err1:
#ifdef _DEBUG
	_RPT1(_CRT_WARN, "%s", buf);
#endif
	return FALSE;
}

const DWORD cBonDriverSplitter::GetCurSpace(void)
{
	return m_dwSpace;
}

const DWORD cBonDriverSplitter::GetCurChannel(void)
{
	return m_dwChannel;
}

DWORD WINAPI cBonDriverSplitter::TsReader(LPVOID pv)
{
	cBonDriverSplitter *pThis = static_cast<cBonDriverSplitter *>(pv);
	DWORD dwSize, dwRemain;
	DWORD &pos = pThis->m_dwPos;
	const DWORD TsPacketBufSize = g_TsPacketBufSize;
	BYTE *pBuf, *pTsBuf = new BYTE[TsPacketBufSize];

	if (g_UseServiceID)
	{
		pThis->m_hTsSplit = ::CreateThread(NULL, 0, TsSplitter, pThis, 0, NULL);
		if (pThis->m_hTsSplit == NULL)
			return 100;
	}

	pos = 0;
	// 内部でCOMを使用しているBonDriverに対する対策
	HRESULT hr = ::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE | COINIT_SPEED_OVER_MEMORY);
	// TS読み込みループ
	while (!pThis->m_bStopTsRead)
	{
		dwSize = dwRemain = 0;
		pThis->m_writeLock.Enter();
		if (pThis->m_pIBon2->GetTsStream(&pBuf, &dwSize, &dwRemain) && (dwSize != 0))
		{
			if ((pos + dwSize) < TsPacketBufSize)
			{
				::memcpy(&pTsBuf[pos], pBuf, dwSize);
				pos += dwSize;
			}
			else
			{
				TS_DATA *pData;
				DWORD left, dwLen = TsPacketBufSize - pos;
				::memcpy(&pTsBuf[pos], pBuf, dwLen);

				pData = new TS_DATA(pTsBuf, TsPacketBufSize);
				if (g_UseServiceID)
					pThis->m_fifoRawTS.Push(pData);
				else
					pThis->m_fifoTS.Push(pData);
				pTsBuf = new BYTE[TsPacketBufSize];

				left = dwSize - dwLen;
				pBuf += dwLen;

				while (left >= TsPacketBufSize)
				{
					::memcpy(pTsBuf, pBuf, TsPacketBufSize);

					pData = new TS_DATA(pTsBuf, TsPacketBufSize);
					if (g_UseServiceID)
						pThis->m_fifoRawTS.Push(pData);
					else
						pThis->m_fifoTS.Push(pData);
					pTsBuf = new BYTE[TsPacketBufSize];

					left -= TsPacketBufSize;
					pBuf += TsPacketBufSize;
				}
				if (left != 0)
					::memcpy(pTsBuf, pBuf, left);
				pos = left;
			}
		}
		pThis->m_writeLock.Leave();
		if (dwRemain == 0)
			::Sleep(WAIT_TIME);
	}
	if (SUCCEEDED(hr))
		::CoUninitialize();
	delete[] pTsBuf;

	if (g_UseServiceID)
	{
		pThis->m_StopTsSplit.Set();
		::WaitForSingleObject(pThis->m_hTsSplit, INFINITE);
		::CloseHandle(pThis->m_hTsSplit);
		pThis->m_hTsSplit = NULL;
		pThis->m_StopTsSplit.Reset();
	}

	return 0;
}

#define MAX_PID	0x2000		// (8 * sizeof(int))で割り切れる
#define PID_SET(pid, map)	((map)->bits[(pid) / (8 * sizeof(int))] |= (1 << ((pid) % (8 * sizeof(int)))))
#define PID_CLR(pid, map)	((map)->bits[(pid) / (8 * sizeof(int))] &= ~(1 << ((pid) % (8 * sizeof(int)))))
#define PID_ISSET(pid, map)	((map)->bits[(pid) / (8 * sizeof(int))] & (1 << ((pid) % (8 * sizeof(int)))))
#define PID_MERGE(dst, src)	{for(int i=0;i<(int)(MAX_PID / (8 * sizeof(int)));i++){(dst)->bits[i] |= (src)->bits[i];}}
#define PID_ZERO(map)		(::memset((map), 0 , sizeof(*(map))))
struct pid_set {
	int bits[MAX_PID / (8 * sizeof(int))];
};

#define FLAG_HEIT	0x0001
#define FLAG_MEIT	0x0002
#define FLAG_LEIT	0x0004
#define FLAG_CAT	0x0008
#define FLAG_NIT	0x0010
#define FLAG_SDT	0x0020
#define FLAG_TOT	0x0040
#define FLAG_SDTT	0x0080
#define FLAG_BIT	0x0100
#define FLAG_CDT	0x0200
#define FLAG_ECM	0x0400
#define FLAG_EMM	0x0800
#define FLAG_TYPED	0x1000
DWORD WINAPI cBonDriverSplitter::TsSplitter(LPVOID pv)
{
	cBonDriverSplitter *pThis = static_cast<cBonDriverSplitter *>(pv);
	BYTE *pTsBuf, pPAT[TS_PKTSIZE];
	BYTE pPMT[4104 + TS_PKTSIZE];	// 4104 = 8(TSヘッダ + pointer_field + table_idからsection_length) + 4096(セクション長最大値)
	BYTE pPMTPackets[TS_PKTSIZE * 32];
	DWORD pos;
	int iNumSplit;
	unsigned char pat_ci, rpmt_ci, wpmt_ci, lpmt_version, lcat_version, ver;
	unsigned short ltsid, pidPMT, pidEMM, pmt_tail;
	BOOL bChangePMT, bSplitPMT, bPMTComplete;
	pid_set pids, save_pids[2], *p_new_pids, *p_old_pids;
	const DWORD TsPacketBufSize = g_TsPacketBufSize;

	pTsBuf = new BYTE[TsPacketBufSize];
	pos = 0;
	pat_ci = 0x10;						// 0x1(payloadのみ) << 4 | 0x0(ci初期値)
	lpmt_version = lcat_version = wpmt_ci = 0xff;
	ltsid = pidPMT = pidEMM = 0xffff;	// 現在のTSID及びPMT,EMMのPID
	bChangePMT = bSplitPMT = bPMTComplete = FALSE;
	PID_ZERO(&pids);
	p_new_pids = &save_pids[0];
	p_old_pids = &save_pids[1];
	PID_ZERO(p_new_pids);
	PID_ZERO(p_old_pids);

	HANDLE h[2] = { pThis->m_StopTsSplit, pThis->m_fifoRawTS.GetEventHandle() };
	for (;;)
	{
		DWORD dwRet = ::WaitForMultipleObjects(2, h, FALSE, INFINITE);
		switch (dwRet)
		{
		case WAIT_OBJECT_0:
			goto end;

		case WAIT_OBJECT_0 + 1:
		{
			TS_DATA *pRawBuf = NULL;
			pThis->m_fifoRawTS.Pop(&pRawBuf);
			if (pRawBuf == NULL)	// イベントのトリガからPop()までの間に別スレッドにFlush()される可能性はゼロではない
				break;
			BYTE *pSrc, *pSrcHead = NULL;
			DWORD dwLeft;
			if (g_TsSync)
			{
				pThis->TsSync(pRawBuf->pbBuf, pRawBuf->dwSize, &pSrcHead, &dwLeft);
				pSrc = pSrcHead;
			}
			else
			{
				pSrc = pRawBuf->pbBuf;
				dwLeft = pRawBuf->dwSize;	// 必ずTS_PKTSIZEの倍数で来る
			}
			while (dwLeft > 0)
			{
				unsigned short pid = GetPID(&pSrc[1]);
				if (pid == 0x0000)	// PAT
				{
					// ビットエラー無しかつpayload先頭かつadaptation_field無し、PSIのpointer_fieldは0x00の前提
					if (!(pSrc[1] & 0x80) && (pSrc[1] & 0x40) && !(pSrc[3] & 0x20) && (pSrc[4] == 0x00))
					{
						// section_length
						// 9 = transport_stream_idからlast_section_numberまでの5バイト + CRC_32の4バイト
						int len = (((int)(pSrc[6] & 0x0f) << 8) | pSrc[7]) - 9;
						// 13 = TSパケットの頭から最初のprogram_numberまでのオフセット
						int off = 13;
						// PATは1TSパケットに収まってる前提
						while ((len >= 4) && ((off + 4) < TS_PKTSIZE))
						{
							unsigned short sid = GetSID(&pSrc[off]);
							if (pThis->m_dwServiceID == sid)
							{
								pid = GetPID(&pSrc[off + 2]);
								break;
							}
							off += 4;
							len -= 4;
						}
						if (pid != 0x0000)	// 対象ServiceIDのPMTのPIDが取得できた
						{
							// transport_stream_id
							unsigned short tsid = ((unsigned short)pSrc[8] << 8) | pSrc[9];
							if (pidPMT != pid || ltsid != tsid)	// PMTのPIDが更新された or チャンネルが変更された
							{
								// TSヘッダ
								pPAT[0] = 0x47;
								pPAT[1] = 0x60;
								pPAT[2] = 0x00;
								pPAT[3] = pat_ci;
								// pointer_field
								pPAT[4] = 0x00;
								// PAT
								pPAT[5] = 0x00;		// table_id
								pPAT[6] = 0xb0;		// section_syntax_indicator(1) + '0'(1) + reserved(2) + section_length(4/12)
								pPAT[7] = 0x11;		// section_length(8/12)
								pPAT[8] = tsid >> 8;
								pPAT[9] = tsid & 0xff;
								pPAT[10] = 0xc1;	// reserved(2) + version_number(5) + current_next_indicator(1)
								pPAT[11] = 0x00;	// section_number
								pPAT[12] = 0x00;	// last_section_number

								pPAT[13] = 0x00;	// program_number(8/16)
								pPAT[14] = 0x00;	// program_number(8/16)
								pPAT[15] = 0xe0;	// reserved(3) + network_PID(5/13)
								pPAT[16] = 0x10;	// network_PID(8/13)

								// 対象ServiceIDのテーブルコピー
								pPAT[17] = pSrc[off];
								pPAT[18] = pSrc[off + 1];
								pPAT[19] = pSrc[off + 2];
								pPAT[20] = pSrc[off + 3];

								// CRC_32
								DWORD crc = CalcCRC32(&pPAT[5], 16);
								pPAT[21] = (BYTE)(crc >> 24);
								pPAT[22] = (BYTE)((crc >> 16) & 0xff);
								pPAT[23] = (BYTE)((crc >> 8) & 0xff);
								pPAT[24] = (BYTE)(crc & 0xff);

								::memset(&pPAT[25], 0xff, TS_PKTSIZE - 25);

								ltsid = tsid;
								pidPMT = pid;
								// PAT更新時には必ずPMT及びCATの更新処理を行う
								lpmt_version = lcat_version = 0xff;
								pidEMM = 0xffff;
								// PATより先に分割PMTの先頭が来ていた場合、そのPMTは破棄
								bSplitPMT = FALSE;
								// なんとなく
								wpmt_ci = 0xff;
							}
							else
							{
								if (pat_ci == 0x1f)
									pat_ci = 0x10;
								else
									pat_ci++;
								pPAT[3] = pat_ci;
							}
							::memcpy(&pTsBuf[pos], pPAT, TS_PKTSIZE);
							pos += TS_PKTSIZE;
						}
					}
				}
				else if (pid == 0x0001)	// CAT
				{
					if (!(g_dwDelFlag & FLAG_CAT))
					{
						// ビットエラー無しかつpayload先頭かつadaptation_field無し、PSIのpointer_fieldは0x00の前提
						if (!(pSrc[1] & 0x80) && (pSrc[1] & 0x40) && !(pSrc[3] & 0x20) && (pSrc[4] == 0x00))
						{
							// version_number
							ver = (pSrc[10] >> 1) & 0x1f;
							if (ver != lcat_version)
							{
								// section_length
								// 9 = 2つ目のreservedからlast_section_numberまでの5バイト + CRC_32の4バイト
								int len = (((int)(pSrc[6] & 0x0f) << 8) | pSrc[7]) - 9;
								// 13 = TSパケットの頭から最初のdescriptorまでのオフセット
								int off = 13;
								// CATも1TSパケットに収まってる前提
								while (len >= 2)
								{
									if ((off + 2) > TS_PKTSIZE)
										break;
									int cdesc_len = 2 + pSrc[off + 1];
									if (cdesc_len > len || (off + cdesc_len) > TS_PKTSIZE)	// descriptor長さ異常
										break;
									if (pSrc[off] == 0x09)	// Conditional Access Descriptor
									{
										if (pSrc[off + 1] >= 4 && (pSrc[off + 4] & 0xe0) == 0xe0)	// 内容が妥当なら
										{
											// EMM PIDセット
											pid = GetPID(&pSrc[off + 4]);
											if (pid != pidEMM)
											{
												if (pidEMM != 0xffff)
													PID_CLR(pidEMM, &pids);
												if (!(g_dwDelFlag & FLAG_EMM))
												{
													PID_SET(pid, &pids);
													pidEMM = pid;
												}
											}
											break;	// EMMが複数のPIDで送られてくる事は無い前提
										}
									}
									off += cdesc_len;
									len -= cdesc_len;
								}
								lcat_version = ver;
							}
							::memcpy(&pTsBuf[pos], pSrc, TS_PKTSIZE);
							pos += TS_PKTSIZE;
						}
					}
				}
				else if (pid == pidPMT)	// PMT
				{
					// ビットエラーがあったら無視
					if (pSrc[1] & 0x80)
						goto next;

					// 分割PMTをまとめる必要が無ければ
					if (!g_ModPMT)
					{
						// とりあえずコピーしてしまう
						::memcpy(&pTsBuf[pos], pSrc, TS_PKTSIZE);
						pos += TS_PKTSIZE;
					}

					int len;
					BYTE *p;
					// payload先頭か？(adaptation_field無し、PSIのpointer_fieldは0x00の前提)
					if ((pSrc[1] & 0x40) && !(pSrc[3] & 0x20) && (pSrc[4] == 0x00))
					{
						// version_number
						ver = (pSrc[10] >> 1) & 0x1f;
						if (ver != lpmt_version)	// バージョンが更新された
						{
							bChangePMT = TRUE;	// PMT更新処理開始
							bSplitPMT = FALSE;
							lpmt_version = ver;
							// 分割PMTをまとめる場合は
							if (g_ModPMT)
							{
								// 送信用PMTも更新を行う
								bPMTComplete = FALSE;
								// 送信用PMT用CI初期値保存
								if (wpmt_ci == 0xff)
									wpmt_ci = (pSrc[3] & 0x0f) | 0x10;
							}
						}
						// PMT更新処理中でなければ何もしない
						// (バージョンチェックのelseにしないのは、分割PMTの処理中にドロップがあった場合などの為)
						if (!bChangePMT)
						{
							// 分割PMTをまとめる場合かつ、送信用PMTができているなら
							if (g_ModPMT && bPMTComplete)
							{
							complete:
								for (int i = 0; i < iNumSplit; i++)
								{
									pPMTPackets[(TS_PKTSIZE * i) + 3] = wpmt_ci;
									if (wpmt_ci == 0x1f)
										wpmt_ci = 0x10;
									else
										wpmt_ci++;
								}
								int sent, left;
								sent = 0;
								left = TS_PKTSIZE * iNumSplit;
								for (;;)
								{
									if ((pos + left) <= TsPacketBufSize)
									{
										::memcpy(&pTsBuf[pos], &pPMTPackets[sent], left);
										pos += left;
										break;
									}
									// バッファサイズが足りない場合
									int diff = (pos + left) - TsPacketBufSize;
									// 入るだけ入れて
									::memcpy(&pTsBuf[pos], &pPMTPackets[sent], (left - diff));
									// キューに投げ込んでから新たにバッファ確保
									TS_DATA *pData = new TS_DATA(pTsBuf, TsPacketBufSize);
									pThis->m_fifoTS.Push(pData);
									pTsBuf = new BYTE[TsPacketBufSize];
									pos = 0;
									// 送信済みサイズ及び残りサイズ更新
									sent += (left - diff);
									left = diff;
								}
							}
							goto next;
						}
						// section_length
						len = (((int)(pSrc[6] & 0x0f) << 8) | pSrc[7]);
						if (len > (TS_PKTSIZE - 8))	// TSパケットを跨ってる
						{
							::memcpy(pPMT, pSrc, TS_PKTSIZE);
							// コピーしたデータの終端位置
							pmt_tail = TS_PKTSIZE;
							bSplitPMT = TRUE;
							rpmt_ci = pSrc[3] & 0x0f;
							if (rpmt_ci == 0x0f)
								rpmt_ci = 0;
							else
								rpmt_ci++;
							goto next;
						}
						// 揃った
						p = pSrc;
					}
					else
					{
						if (!bChangePMT)	// PMT更新処理中でなければ
							goto next;
						if (!bSplitPMT)		// 分割PMTの続き待ち中でなければ
							goto next;
						// CIが期待している値ではない、もしくはpayloadが無い場合
						if (((pSrc[3] & 0x0f) != rpmt_ci) || !(pSrc[3] & 0x10))
						{
							// 最初からやり直し
							bSplitPMT = FALSE;
							goto next;
						}
						unsigned short adplen;
						if (pSrc[3] & 0x20)	// adaptation_field有り(まあ無いとは思うけど一応)
						{
							adplen = pSrc[4] + 1;
							if (adplen >= (TS_PKTSIZE - 4))
							{
								// adaptation_fieldの長さが異常なので最初からやり直し
								bSplitPMT = FALSE;
								goto next;
							}
						}
						else
							adplen = 0;
						// 分割PMTの続きコピー
						// pPMTのサイズはTS_PKTSIZEバイト余分に確保しているのでこれでも大丈夫
						::memcpy(&pPMT[pmt_tail], &pSrc[4 + adplen], TS_PKTSIZE - 4 - adplen);
						// section_length
						len = (((int)(pPMT[6] & 0x0f) << 8) | pPMT[7]);
						if (len > (pmt_tail - 8 + (TS_PKTSIZE - 4 - adplen)))	// まだ全部揃ってない
						{
							pmt_tail += (TS_PKTSIZE - 4 - adplen);
							if (rpmt_ci == 0x0f)
								rpmt_ci = 0;
							else
								rpmt_ci++;
							goto next;
						}
						// 揃った
						p = pPMT;
					}
					// この時点でセクションは必ず揃っている
					int limit = 8 + len;
					// 新PIDマップ初期化
					PID_ZERO(p_new_pids);
					// PMT PIDセット(マップにセットしても意味無いけど一応)
					PID_SET(pidPMT, p_new_pids);
					if (!(g_dwDelFlag & FLAG_NIT))
						PID_SET(0x0010, p_new_pids);	// NIT PIDセット
					if (!(g_dwDelFlag & FLAG_SDT))
						PID_SET(0x0011, p_new_pids);	// SDT PIDセット
					if (!(g_dwDelFlag & FLAG_HEIT))
						PID_SET(0x0012, p_new_pids);	// H-EIT PIDセット
					if (!(g_dwDelFlag & FLAG_TOT))
						PID_SET(0x0014, p_new_pids);	// TOT PIDセット
					if (!(g_dwDelFlag & FLAG_SDTT))
						PID_SET(0x0023, p_new_pids);	// SDTT PIDセット
					if (!(g_dwDelFlag & FLAG_BIT))
						PID_SET(0x0024, p_new_pids);	// BIT PIDセット
					if (!(g_dwDelFlag & FLAG_MEIT))
						PID_SET(0x0026, p_new_pids);	// M-EIT PIDセット
					if (!(g_dwDelFlag & FLAG_LEIT))
						PID_SET(0x0027, p_new_pids);	// L-EIT PIDセット
					if (!(g_dwDelFlag & FLAG_CDT))
						PID_SET(0x0029, p_new_pids);	// CDT PIDセット
					if (pidEMM != 0xffff)				// FLAG_EMMが立っている時はpidEMMは必ず0xffff
						PID_SET(pidEMM, p_new_pids);	// EMM PIDセット
					// PCR PIDセット
					pid = GetPID(&p[13]);
					if (pid != 0x1fff)
						PID_SET(pid, p_new_pids);
					// program_info_length
					int desc_len = (((int)(p[15] & 0x0f) << 8) | p[16]);
					// 17 = 最初のdescriptorのオフセット
					int off = 17;
					int left = desc_len;
					while (left >= 2)
					{
						if ((off + 2) > limit)	// program_info_length異常
						{
							bSplitPMT = FALSE;
							goto next;
						}
						int cdesc_len = 2 + p[off + 1];
						if (cdesc_len > left || (off + cdesc_len) > limit)	// descriptor長さ異常
						{
							bSplitPMT = FALSE;
							goto next;
						}
						if (p[off] == 0x09)	// Conditional Access Descriptor
						{
							if (p[off + 1] >= 4 && (p[off + 4] & 0xe0) == 0xe0)	// 内容が妥当なら
							{
								// ECM PIDセット(第1ループに無効ECMは来ない / ARIB TR-B14/B15)
								pid = GetPID(&p[off + 4]);
								if (!(g_dwDelFlag & FLAG_ECM))
									PID_SET(pid, p_new_pids);
							}
						}
						off += cdesc_len;
						left -= cdesc_len;
					}
					// データ異常が無ければ必要無いが一応
					off = 17 + desc_len;
					// 13 = program_numberからprogram_info_lengthまでの9バイト + CRC_32の4バイト
					len -= (13 + desc_len);
					while (len >= 5)
					{
						if ((off + 5) > limit)	// program_info_length異常
						{
							bSplitPMT = FALSE;
							goto next;
						}
						if ((p[off] != 0x0d) || !(g_dwDelFlag & FLAG_TYPED))	// stream_type "ISO/IEC 13818-6 type D"以外は無条件で残す
						{
							pid = GetPID(&p[off + 1]);
							PID_SET(pid, p_new_pids);
						}
						// ES_info_length
						desc_len = (((int)(p[off + 3] & 0x0f) << 8) | p[off + 4]);
						// 5 = 最初のdescriptorのオフセット
						int coff = off + 5;
						left = desc_len;
						while (left >= 2)
						{
							if ((coff + 2) > limit)	// ES_info_length異常
							{
								bSplitPMT = FALSE;
								goto next;
							}
							int cdesc_len = 2 + p[coff + 1];
							if (cdesc_len > left || (coff + cdesc_len) > limit)	// descriptor長さ異常
							{
								bSplitPMT = FALSE;
								goto next;
							}
							if (p[coff] == 0x09)	// Conditional Access Descriptor
							{
								if (p[coff + 1] >= 4 && (p[coff + 4] & 0xe0) == 0xe0)	// 内容が妥当なら
								{
									// ECM PIDセット
									pid = GetPID(&p[coff + 4]);
									if (pid != 0x1fff)
									{
										if (!(g_dwDelFlag & FLAG_ECM))
											PID_SET(pid, p_new_pids);
									}
								}
							}
							coff += cdesc_len;
							left -= cdesc_len;
						}
						// 5 = stream_typeからES_info_lengthまでの5バイト
						off += (5 + desc_len);
						len -= (5 + desc_len);
					}
					// section_length
					len = (((int)(p[6] & 0x0f) << 8) | p[7]);
					// CRC_32チェック
					// 3 = table_idからsection_lengthまでの3バイト
					if (CalcCRC32(&p[5], len + 3) == 0)
					{
						// 新PIDマップを適用
						::memcpy(&pids, p_new_pids, sizeof(pids));
						// チャンネル変更でなければ
						if (!pThis->m_bChannelChanged)
						{
							// 旧PIDマップをマージ
							PID_MERGE(&pids, p_old_pids);
						}
						else
							pThis->m_bChannelChanged = FALSE;
						// 次回は今回のPMTで示されたPIDを旧PIDマップとする
						pid_set *p_tmp_pids;
						p_tmp_pids = p_old_pids;
						p_old_pids = p_new_pids;
						p_new_pids = p_tmp_pids;
						// PMT更新処理完了
						bChangePMT = bSplitPMT = FALSE;
						// 分割PMTをまとめる場合は、送信用PMTパケット作成
						if (g_ModPMT)
						{
							// TSヘッダを除いた残りデータサイズ
							// 4 = pointer_fieldの1バイト + 上のと同じ3バイト
							int left = 4 + len;
							// このPMTをいくつのTSパケットに分割する必要があるか
							iNumSplit = ((left - 1) / (TS_PKTSIZE - 4)) + 1;
							::memset(pPMTPackets, 0xff, (TS_PKTSIZE * iNumSplit));
							for (int i = 0; i < iNumSplit; i++)
							{
								// TSヘッダの4バイト分をコピー
								::memcpy(&pPMTPackets[TS_PKTSIZE * i], p, 4);
								// 先頭パケット以外はunit_start_indicatorを外す
								if (i != 0)
									pPMTPackets[(TS_PKTSIZE * i) + 1] &= ~0x40;
								int n;
								if (left >(TS_PKTSIZE - 4))
									n = TS_PKTSIZE - 4;
								else
									n = left;
								::memcpy(&pPMTPackets[(TS_PKTSIZE * i) + 4], &p[4 + ((TS_PKTSIZE - 4) * i)], n);
								left -= n;
							}
							bPMTComplete = TRUE;
							// まずこのパケットを送信
							goto complete;
						}
					}
					else
					{
						// CRC_32チェックエラーなので最初からやり直し
						bSplitPMT = FALSE;
					}
				}
				else
				{
					if (PID_ISSET(pid, &pids))
					{
						::memcpy(&pTsBuf[pos], pSrc, TS_PKTSIZE);
						pos += TS_PKTSIZE;
					}
				}

			next:
				pSrc += TS_PKTSIZE;
				dwLeft -= TS_PKTSIZE;

				// 1ループでのposの増加は0もしくはTS_PKTSIZEなので、
				// バウンダリチェックはこれで大丈夫なハズ
				if (pos == TsPacketBufSize)
				{
					TS_DATA *pData = new TS_DATA(pTsBuf, TsPacketBufSize);
					pThis->m_fifoTS.Push(pData);
					pTsBuf = new BYTE[TsPacketBufSize];
					pos = 0;
				}
			}
			if (g_TsSync)
				delete[] pSrcHead;
			delete pRawBuf;
		}
		}
	}
end:
	delete[] pTsBuf;
	return NULL;
}

BOOL cBonDriverSplitter::TsSync(BYTE *pSrc, DWORD dwSrc, BYTE **ppDst, DWORD *pdwDst)
{
	// 同期チェックの開始位置
	DWORD dwCheckStartPos = 0;
	// 既に同期済みか？
	if (m_dwUnitSize != 0)
	{
		for (DWORD pos = m_dwUnitSize - m_dwSyncBufPos; pos < dwSrc; pos += m_dwUnitSize)
		{
			if (pSrc[pos] != TS_SYNC_BYTE)
			{
				// 今回の入力バッファで同期が崩れてしまうので要再同期
				m_dwUnitSize = 0;
				// 今回の入力バッファの先頭から同期の崩れた場所までは破棄する事になる
				dwCheckStartPos = pos;
				goto resync;
			}
		}
		DWORD dwDst = TS_PKTSIZE * (((m_dwSyncBufPos + dwSrc) - 1) / m_dwUnitSize);
		if (dwDst == 0)
		{
			// 同期用繰り越しバッファと今回の入力バッファを合わせてもユニットサイズ+1に
			// 届かなかった(==次の同期バイトのチェックが行えなかった)ので、
			// 今回の入力バッファを同期用繰り越しバッファに追加するだけで終了
			::memcpy(&m_SyncBuf[m_dwSyncBufPos], pSrc, dwSrc);
			m_dwSyncBufPos += dwSrc;
			*ppDst = NULL;	// 呼び出し側でのdelete[]を保証する
			*pdwDst = 0;
			return FALSE;
		}
		BYTE *pDst = new BYTE[dwDst];
		if (m_dwSyncBufPos >= TS_PKTSIZE)
			::memcpy(pDst, m_SyncBuf, TS_PKTSIZE);
		else
		{
			if (m_dwSyncBufPos == 0)
				::memcpy(pDst, pSrc, TS_PKTSIZE);
			else
			{
				::memcpy(pDst, m_SyncBuf, m_dwSyncBufPos);
				::memcpy(&pDst[m_dwSyncBufPos], pSrc, TS_PKTSIZE - m_dwSyncBufPos);
			}
		}
		DWORD dwSrcPos = m_dwUnitSize - m_dwSyncBufPos;
		if (m_dwUnitSize == TS_PKTSIZE)
		{
			// 普通のTSパケットの場合はそのままコピーできる
			if ((dwDst - TS_PKTSIZE) != 0)
			{
				::memcpy(&pDst[TS_PKTSIZE], &pSrc[dwSrcPos], (dwDst - TS_PKTSIZE));
				dwSrcPos += (dwDst - TS_PKTSIZE);
			}
		}
		else
		{
			// それ以外のパケットの場合は普通のTSパケットに変換
			for (DWORD pos = TS_PKTSIZE; (dwSrcPos + m_dwUnitSize) < dwSrc; dwSrcPos += m_dwUnitSize, pos += TS_PKTSIZE)
				::memcpy(&pDst[pos], &pSrc[dwSrcPos], TS_PKTSIZE);
		}
		if ((dwSrc - dwSrcPos) != 0)
		{
			// 入力バッファに余りがあるので同期用繰り越しバッファに保存
			::memcpy(m_SyncBuf, &pSrc[dwSrcPos], (dwSrc - dwSrcPos));
			m_dwSyncBufPos = dwSrc - dwSrcPos;
		}
		else
			m_dwSyncBufPos = 0;
		*ppDst = pDst;
		*pdwDst = dwDst;
		return TRUE;
	}

resync:
	// 同期処理開始
	DWORD dwSyncBufPos = m_dwSyncBufPos;
	for (DWORD off = dwCheckStartPos; (off + TS_PKTSIZE) < (dwSyncBufPos + dwSrc); off++)
	{
		if (((off >= dwSyncBufPos) && (pSrc[off - dwSyncBufPos] == TS_SYNC_BYTE)) || ((off < dwSyncBufPos) && (m_SyncBuf[off] == TS_SYNC_BYTE)))
		{
			for (int type = 0; type < 4; type++)
			{
				DWORD dwUnitSize;
				switch (type)
				{
				case 0:
					dwUnitSize = TS_PKTSIZE;
					break;
				case 1:
					dwUnitSize = TTS_PKTSIZE;
					break;
				case 2:
					dwUnitSize = TS_FEC_PKTSIZE;
					break;
				default:
					dwUnitSize = TTS_FEC_PKTSIZE;
					break;
				}
				BOOL bSync = TRUE;
				// 次の同期バイトが同期用繰り越しバッファ内に含まれている可能性があるか？
				if (dwUnitSize >= dwSyncBufPos)
				{
					// なかった場合は同期用繰り越しバッファのチェックは不要
					DWORD pos = off + (dwUnitSize - dwSyncBufPos);
					if (pos >= dwSrc)
					{
						// bSync = FALSE;
						// これ以降のユニットサイズではこの場所で同期成功する事は無いのでbreak
						break;
					}
					else
					{
						// 同一ユニットサイズのバッファが8個もしくは今回の入力バッファの
						// 最後まで並んでいるなら同期成功とみなす
						int n = 0;
						do
						{
							if (pSrc[pos] != TS_SYNC_BYTE)
							{
								bSync = FALSE;
								break;
							}
							pos += dwUnitSize;
							n++;
						} while ((n < 8) && (pos < dwSrc));
					}
				}
				else
				{
					DWORD pos = off + dwUnitSize;
					if (pos >= (dwSyncBufPos + dwSrc))
					{
						// bSync = FALSE;
						// これ以降のユニットサイズではこの場所で同期成功する事は無いのでbreak
						break;
					}
					else
					{
						// 同一ユニットサイズのバッファが8個もしくは今回の入力バッファの
						// 最後まで並んでいるなら同期成功とみなす
						int n = 0;
						do
						{
							if (((pos >= dwSyncBufPos) && (pSrc[pos - dwSyncBufPos] != TS_SYNC_BYTE)) || ((pos < dwSyncBufPos) && (m_SyncBuf[pos] != TS_SYNC_BYTE)))
							{
								bSync = FALSE;
								break;
							}
							pos += dwUnitSize;
							n++;
						} while ((n < 8) && (pos < (dwSyncBufPos + dwSrc)));
					}
				}
				if (bSync)
				{
					m_dwUnitSize = dwUnitSize;
					if (off < dwSyncBufPos)
					{
						if (off != 0)
						{
							dwSyncBufPos -= off;
							::memmove(m_SyncBuf, &m_SyncBuf[off], dwSyncBufPos);
						}
						// この同期検出ロジックでは↓の状態は起こり得ないハズ
#if 0
						// 同期済み時の同期用繰り越しバッファサイズはユニットサイズ以下である必要がある
						if (dwSyncBufPos > dwUnitSize)
						{
							dwSyncBufPos -= dwUnitSize;
							::memmove(m_SyncBuf, &m_SyncBuf[dwUnitSize], dwSyncBufPos);
						}
#endif
						m_dwSyncBufPos = dwSyncBufPos;
						return TsSync(pSrc, dwSrc, ppDst, pdwDst);
					}
					else
					{
						m_dwSyncBufPos = 0;
						return TsSync(&pSrc[off - dwSyncBufPos], (dwSrc - (off - dwSyncBufPos)), ppDst, pdwDst);
					}
				}
			}
		}
	}

	// 今回の入力では同期できなかったので、同期用繰り越しバッファに保存だけして終了
	if (dwSrc >= sizeof(m_SyncBuf))
	{
		::memcpy(m_SyncBuf, &pSrc[dwSrc - sizeof(m_SyncBuf)], sizeof(m_SyncBuf));
		m_dwSyncBufPos = sizeof(m_SyncBuf);
	}
	else if ((dwSyncBufPos + dwSrc) > sizeof(m_SyncBuf))
	{
		::memmove(m_SyncBuf, &m_SyncBuf[(dwSyncBufPos + dwSrc) - sizeof(m_SyncBuf)], (sizeof(m_SyncBuf) - dwSrc));
		::memcpy(&m_SyncBuf[sizeof(m_SyncBuf) - dwSrc], pSrc, dwSrc);
		m_dwSyncBufPos = sizeof(m_SyncBuf);
	}
	else
	{
		::memcpy(&m_SyncBuf[dwSyncBufPos], pSrc, dwSrc);
		m_dwSyncBufPos += dwSrc;
	}
	*ppDst = NULL;	// 呼び出し側でのdelete[]を保証する
	*pdwDst = 0;
	return FALSE;
}

cBonDriverSplitter *cBonDriverSplitter::m_spThis;
cCriticalSection cBonDriverSplitter::m_sInstanceLock;

extern "C" __declspec(dllexport) IBonDriver *CreateBonDriver()
{
	cBonDriverSplitter *pSplitter = NULL;
	cBonDriverSplitter::m_sInstanceLock.Enter();
	if (cBonDriverSplitter::m_spThis == NULL)
		pSplitter = new cBonDriverSplitter();
	cBonDriverSplitter::m_sInstanceLock.Leave();
	return pSplitter;
}

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID/*lpvReserved*/)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
	{
#ifdef _DEBUG
		_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
		_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_RPT0(_CRT_WARN, "--- BonDriver_Splitter / DLL_PROCESS_ATTACH ---\n");
#endif
		cBonDriverSplitter::m_spThis = NULL;
		if (Init(hinstDLL) != 0)
			return FALSE;
		break;
	}

	case DLL_PROCESS_DETACH:
	{
		cBonDriverSplitter::m_sInstanceLock.Enter();
		if (cBonDriverSplitter::m_spThis != NULL)
			delete cBonDriverSplitter::m_spThis;
		cBonDriverSplitter::m_sInstanceLock.Leave();
#ifdef _DEBUG
		_RPT0(_CRT_WARN, "--- BonDriver_Splitter / DLL_PROCESS_DETACH ---\n");
#endif
		break;
	}
	}
	return TRUE;
}
