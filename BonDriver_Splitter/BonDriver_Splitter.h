#ifndef __BONDRIVER_SPLITTER_H__
#define __BONDRIVER_SPLITTER_H__
#include <windows.h>
#include <tchar.h>
#include <process.h>
#include <queue>
#include <vector>
#include "Common.h"
#include "IBonDriver2.h"

#if _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#define new new(_NORMAL_BLOCK, __FILE__, __LINE__)
#endif

#define TUNER_NAME	L"BonDriverSplitter"

#define WAIT_TIME			10		// GetTsStream()ÇÃå„Ç≈ÅAdwRemainÇ™0ÇæÇ¡ÇΩèÍçáÇ…ë“Ç¬éûä‘(ms)

#define TS_SYNC_BYTE		0x47
#define TS_PKTSIZE			188
#define TTS_PKTSIZE			192
#define TS_FEC_PKTSIZE		204
#define TTS_FEC_PKTSIZE		208

#define MAX_DRIVER			16		// 99à»â∫
#define MAX_SPACE			16		// 99à»â∫
#define MAX_SPACE_LEN		64
#define MAX_CH				128		// 999à»â∫
#define MAX_CN_LEN			64

struct stChannel {
	WCHAR ChName[MAX_CN_LEN];
	int BonNo;
	DWORD BonSpace;
	DWORD BonChannel;
	DWORD ServiceID;
};

struct stSpace {
	WCHAR SpaceName[MAX_SPACE_LEN];
	std::vector<stChannel> vstChannel;
};

////////////////////////////////////////////////////////////////////////////////

static std::vector<std::string> g_vBonDrivers;
static std::vector<stSpace> g_vstSpace;
static size_t g_TsFifoSize;
static DWORD g_TsPacketBufSize;
static BOOL g_UseServiceID;
static DWORD g_Crc32Table[256];
static BOOL g_ModPMT;
static BOOL g_TsSync;
static DWORD g_dwDelFlag;

////////////////////////////////////////////////////////////////////////////////

struct TS_DATA {
	BYTE *pbBuf;
	DWORD dwSize;
	TS_DATA(BYTE *pb, DWORD dw) : pbBuf(pb), dwSize(dw){}
	~TS_DATA(){	delete[] pbBuf;	}
};

class cTSFifo : protected std::queue<TS_DATA *> {
	const size_t m_fifoSize;
	cCriticalSection m_Lock;
	cEvent m_Event;
	cTSFifo &operator=(const cTSFifo &);	// shut up C4512

public:
	cTSFifo() : m_fifoSize(g_TsFifoSize), m_Event(TRUE, FALSE){}
	~cTSFifo(){ Flush(); }

	void Flush()
	{
		LOCK(m_Lock);
		while (!empty())
		{
			TS_DATA *p = front();
			pop();
			delete p;
		}
		m_Event.Reset();
	}

	void Push(TS_DATA *p)
	{
		LOCK(m_Lock);
		if (size() >= m_fifoSize)
		{
#if _DEBUG
			_RPT1(_CRT_WARN, "TS Queue OVERFLOW : size[%d]\n", size());
#endif
			TS_DATA *pDel = front();
			pop();
			delete pDel;
		}
		push(p);
		m_Event.Set();
	}

	void Pop(TS_DATA **p)
	{
		LOCK(m_Lock);
		if (!empty())
		{
			*p = front();
			pop();
			if (empty())
				m_Event.Reset();
		}
		else
			m_Event.Reset();
	}

	HANDLE GetEventHandle()
	{
		return (HANDLE)m_Event;
	}

	inline size_t Size()
	{
		return size();
	}
};

////////////////////////////////////////////////////////////////////////////////

class cBonDriverSplitter : public IBonDriver2 {
	HMODULE m_hBonModule;
	IBonDriver2 *m_pIBon2;
	cEvent m_eCloseTuner;
	cCriticalSection m_bonLock;
	cCriticalSection m_writeLock;
	cTSFifo m_fifoTS;
	cTSFifo m_fifoRawTS;
	TS_DATA *m_LastBuf;
	BOOL m_bTuner;
	int m_iBonNo;
	DWORD m_dwSpace;
	DWORD m_dwChannel;
	DWORD m_dwServiceID;
	HANDLE m_hTsRead;
	HANDLE m_hTsSplit;
	volatile BOOL m_bStopTsRead;
	volatile BOOL m_bChannelChanged;
	DWORD m_dwPos;
	cEvent m_StopTsSplit;
	DWORD m_dwUnitSize;
	DWORD m_dwSyncBufPos;
	BYTE m_SyncBuf[256];

	void TsFlush(BOOL bUseServiceID)
	{
		m_fifoTS.Flush();
		if (bUseServiceID)
			m_fifoRawTS.Flush();
		m_dwPos = 0;
	}
	static DWORD WINAPI TsReader(LPVOID pv);
	static DWORD WINAPI TsSplitter(LPVOID pv);
	BOOL TsSync(BYTE *pSrc, DWORD dwSrc, BYTE **ppDst, DWORD *pdwDst);
	static inline unsigned short GetPID(BYTE *p){ return (((unsigned short)(p[0] & 0x1f) << 8) | p[1]); }
	static inline unsigned short GetSID(BYTE *p){ return (((unsigned short)p[0] << 8) | p[1]); }

public:
	static cBonDriverSplitter *m_spThis;
	static cCriticalSection m_sInstanceLock;

	cBonDriverSplitter();
	~cBonDriverSplitter();

	// IBonDriver
	const BOOL OpenTuner(void);
	void CloseTuner(void);
	const BOOL SetChannel(const BYTE bCh);
	const float GetSignalLevel(void);
	const DWORD WaitTsStream(const DWORD dwTimeOut = 0);
	const DWORD GetReadyCount(void);
	const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain);
	const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain);
	void PurgeTsStream(void);
	void Release(void);

	// IBonDriver2
	LPCTSTR GetTunerName(void);
	const BOOL IsTunerOpening(void);
	LPCTSTR EnumTuningSpace(const DWORD dwSpace);
	LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel);
	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel);
	const DWORD GetCurSpace(void);
	const DWORD GetCurChannel(void);
};

#endif	// __BONDRIVER_SPLITTER_H__
