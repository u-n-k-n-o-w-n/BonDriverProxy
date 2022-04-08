#ifndef __BONDRIVER_PROXY_H__
#define __BONDRIVER_PROXY_H__
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tchar.h>
#include <process.h>
#include <list>
#include <queue>
#include "Common.h"
#include "IBonDriver3.h"

#if _DEBUG
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#define new new(_NORMAL_BLOCK, __FILE__, __LINE__)
#endif

#define TUNER_NAME	"BonDriverProxy"

////////////////////////////////////////////////////////////////////////////////

#define MAX_HOST_LEN	256
#define MAX_PORT_LEN	8
static char g_Host[MAX_HOST_LEN];
static char g_Port[MAX_PORT_LEN];
static char g_BonDriver[MAX_PATH];
static BYTE g_ChannelLock;
static size_t g_PacketFifoSize;
static size_t g_TsFifoSize;
static DWORD g_TsPacketBufSize;
static int g_ConnectTimeOut;
static UINT g_UseMagicPacket;
static UINT g_UsePatternMatch;
static char g_TargetMac[6];
static char g_TargetHost[MAX_HOST_LEN];
static char g_TargetPort[MAX_PORT_LEN];

#include "BdpPacket.h"

////////////////////////////////////////////////////////////////////////////////

struct TS_DATA {
	BYTE *pbBufHead;
	BYTE *pbBuf;
	DWORD dwSize;
	TS_DATA(void)
	{
		pbBufHead = pbBuf = NULL;
		dwSize = 0;
	}
	~TS_DATA(void)
	{
		delete[] pbBufHead;
	}
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

enum enumbRes {
	ebResSelectBonDriver = 0,
	ebResCreateBonDriver,
	ebResOpenTuner,
	ebResPurgeTsStream,
	ebResSetLnbPower,
	ebResNum,
};

enum enumdwRes {
	edwResSetChannel2 = 0,
	edwResGetTotalDeviceNum,
	edwResGetActiveDeviceNum,
	edwResNum,
};

enum enumpRes {
	epResEnumTuningSpace = 0,
	epResEnumChannelName,
	epResNum,
};

class cProxyClient : public IBonDriver3 {
	SOCKET m_s;
//	HANDLE m_hThread;
	__declspec(align(4)) volatile LONG m_lEndCount;
	cEvent m_Error;
	cEvent m_StartTrigger;
	cPacketFifo m_fifoSend;
	cPacketFifo m_fifoRecv;
	cTSFifo m_fifoTS;
	TS_DATA *m_LastBuf;
	cEvent m_bResEvent[ebResNum];
	BOOL m_bRes[ebResNum];
	cEvent m_dwResEvent[edwResNum];
	DWORD m_dwRes[edwResNum];
	cEvent m_pResEvent[epResNum];
	TCHAR *m_pRes[epResNum];
	DWORD m_dwBufPos;
	TCHAR *m_pBuf[8];
	float m_fSignalLevel;
	DWORD m_dwSpace;
	DWORD m_dwChannel;
	BOOL m_bBonDriver;
	BOOL m_bTuner;
	BOOL m_bRereased;
	volatile BOOL m_bWaitCNR;
	cCriticalSection m_writeLock;
	cCriticalSection m_readLock;	// �ꉞ���b�N���Ă邯�ǁA�����ɂ͖{�����߂Ă郍�b�N�͕ۏ؂ł��ĂȂ�

	DWORD Process();
	int ReceiverHelper(char *pDst, DWORD left);
	static DWORD WINAPI Receiver(LPVOID pv);
	void makePacket(enumCommand eCmd);
	void makePacket(enumCommand eCmd, LPCSTR);
	void makePacket(enumCommand eCmd, BOOL b);
	void makePacket(enumCommand eCmd, DWORD dw);
	void makePacket(enumCommand eCmd, DWORD dw1, DWORD dw2);
	void makePacket(enumCommand eCmd, DWORD dw1, DWORD dw2, BYTE b);
	static DWORD WINAPI Sender(LPVOID pv);
	void TsFlush(){ m_fifoTS.Flush(); }
	void SleepLock(LONG n){ while (m_lEndCount != n){ ::Sleep(1); }; }

public:
	cProxyClient();
	~cProxyClient();
	void setSocket(SOCKET s){ m_s = s; }
//	void setThreadHandle(HANDLE h){ m_hThread = h; }
	DWORD WaitStartTrigger(){ return m_StartTrigger.Wait(m_Error); }
	static DWORD WINAPI ProcessEntry(LPVOID pv);

	BOOL SelectBonDriver();
	BOOL CreateBonDriver();

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

	// IBonDriver3
	const DWORD GetTotalDeviceNum(void);
	const DWORD GetActiveDeviceNum(void);
	const BOOL SetLnbPower(const BOOL bEnable);
};

static std::list<cProxyClient *> g_InstanceList;
static cCriticalSection g_Lock;
static BOOL g_bWinSockInit = TRUE;

#endif	// __BONDRIVER_PROXY_H__
