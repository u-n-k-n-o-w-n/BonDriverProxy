#ifndef __COMMON_H__
#define __COMMON_H__
#include <windows.h>

class cCriticalSection {
	CRITICAL_SECTION m_c;
public:
	cCriticalSection(){ ::InitializeCriticalSection(&m_c); }
	~cCriticalSection(){ ::DeleteCriticalSection(&m_c); }
	void Enter(){ ::EnterCriticalSection(&m_c); }
	void Leave(){ ::LeaveCriticalSection(&m_c); }
};

class cLock {
	cCriticalSection &m_c;
	cLock &operator=(const cLock &);	// shut up C4512
public:
	cLock(cCriticalSection &ref) : m_c(ref) { m_c.Enter(); }
	~cLock(){ m_c.Leave(); }
};

#define LOCK(key) cLock __Lock__(key)

class cEvent {
	HANDLE m_h;
	DWORD m_dwWait;
public:
	cEvent(BOOL bManualReset = FALSE, BOOL bInitialState = FALSE, DWORD dwMilliseconds = INFINITE)
	{
		m_dwWait = dwMilliseconds;
		m_h = ::CreateEvent(NULL, bManualReset, bInitialState, NULL);
	}
	~cEvent(){ ::CloseHandle(m_h); }
	BOOL IsSet(){ return (::WaitForSingleObject(m_h, 0) == WAIT_OBJECT_0); }
	DWORD Wait(HANDLE err_h)
	{
		HANDLE h[2] = { err_h, m_h };
		return ::WaitForMultipleObjects(2, h, FALSE, m_dwWait);
	}
	BOOL Set(){ return ::SetEvent(m_h); }
	BOOL Reset(){ return ::ResetEvent(m_h); }
	operator HANDLE () const { return m_h; }
};

#endif	// __COMMON_H__
