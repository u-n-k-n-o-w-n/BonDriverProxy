#ifndef __BONDRIVERPROXY_H__
#define __BONDRIVERPROXY_H__
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

#define WAIT_TIME	10	// GetTsStream()ÇÃå„Ç≈ÅAdwRemainÇ™0ÇæÇ¡ÇΩèÍçáÇ…ë“Ç¬éûä‘(ms)

////////////////////////////////////////////////////////////////////////////////

static char g_Host[256];
static char g_Port[8];
static size_t g_PacketFifoSize;
static DWORD g_TsPacketBufSize;
static BOOL g_SandBoxedRelease;

#include "BdpPacket.h"

////////////////////////////////////////////////////////////////////////////////

class cProxyServer {
	IBonDriver *m_pIBon;
	IBonDriver2 *m_pIBon2;
	IBonDriver3 *m_pIBon3;
	HMODULE m_hModule;
	SOCKET m_s;
	cEvent m_Error;
	BOOL m_bTunerOpen;
	HANDLE m_hTsRead;
	std::list<cProxyServer *> *m_pTsReceiversList;
	BOOL * volatile m_pStopTsRead;
	cCriticalSection *m_pTsLock;
	DWORD *m_ppos;
	BOOL m_bChannelLock;
#if _DEBUG
public:
#endif
	char m_strBonDriver[MAX_PATH];
	cPacketFifo m_fifoSend;
	cPacketFifo m_fifoRecv;

#if _DEBUG
private:
#endif
	DWORD Process();
	int ReceiverHelper(char *pDst, DWORD left);
	static DWORD WINAPI Receiver(LPVOID pv);
	void makePacket(enumCommand eCmd, BOOL b);
	void makePacket(enumCommand eCmd, DWORD dw);
	void makePacket(enumCommand eCmd, LPCTSTR str);
	void makePacket(enumCommand eCmd, BYTE *pSrc, DWORD dwSize, float fSignalLevel);
	static DWORD WINAPI Sender(LPVOID pv);
	static DWORD WINAPI TsReader(LPVOID pv);

	BOOL SelectBonDriver(LPCSTR p);
	IBonDriver *CreateBonDriver();

	// IBonDriver
	const BOOL OpenTuner(void);
	void CloseTuner(void);
	void PurgeTsStream(void);
	void Release(void);

	// IBonDriver2
	LPCTSTR EnumTuningSpace(const DWORD dwSpace);
	LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel);
	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel);

	// IBonDriver3
	const DWORD GetTotalDeviceNum(void);
	const DWORD GetActiveDeviceNum(void);
	const BOOL SetLnbPower(const BOOL bEnable);

public:
	cProxyServer();
	~cProxyServer();
	void setSocket(SOCKET s){ m_s = s; }
	static DWORD WINAPI Reception(LPVOID pv);
};

static std::list<cProxyServer *> g_InstanceList;
static cCriticalSection g_Lock;

#endif	// __BONDRIVERPROXY_H__
