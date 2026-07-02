/*
  apt.c _ Applet/NS shell interaction
*/

#include <stdlib.h>
#include <string.h>
#include <3ds/types.h>
#include <3ds/result.h>
#include <3ds/svc.h>
#include <3ds/srv.h>
#include <3ds/synchronization.h>
#include <3ds/services/apt.h>
#include <3ds/services/gspgpu.h>
#include <3ds/services/ptmsysm.h> // for PtmWakeEvents
#include <3ds/allocator/mappable.h>
#include <3ds/ipc.h>
#include <3ds/env.h>
#include <3ds/thread.h>
#include <3ds/os.h>

#define APT_HANDLER_STACKSIZE (0x1000)

#define APT_EVENT_NOTIFICATION_INDEX 0
#define APT_EVENT_RESUME_INDEX 1

static int aptRefCount = 0;
static Handle aptLockHandle;
static Handle aptEvents[2];
static LightEvent aptReceiveEvent;
static LightEvent aptSleepEvent;
static Thread aptEventHandlerThread;
static bool aptEventHandlerThreadQuit;
static aptHookCookie aptFirstHook;
static aptMessageCb aptMessageFunc;
static aptSignalCb aptSignalFunc;
static void* aptMessageFuncData;

enum
{
	// Current applet state
	FLAG_ACTIVE       = BIT(0),
	FLAG_SLEEPING     = BIT(1),

	// Sleep handling flags
	FLAG_ALLOWSLEEP   = BIT(2),
	FLAG_SHOULDSLEEP  = BIT(3),

	// Home button flags
	FLAG_ALLOWHOME    = BIT(4),
	FLAG_SHOULDHOME   = BIT(5),
	FLAG_HOMEREJECTED = BIT(6),

	// Power button flags
	FLAG_POWERBUTTON  = BIT(7),
	FLAG_SHUTDOWN     = BIT(8),

	// Close handling flags
	FLAG_ORDERTOCLOSE = BIT(9),
	FLAG_CANCELLED    = BIT(10),

	// Miscellaneous
	FLAG_DSPWAKEUP    = BIT(29),
	FLAG_CHAINLOAD    = BIT(30),
	FLAG_SPURIOUS     = BIT(31),
};

static u32 aptFlags;
static u32 aptParameters[0x1000/4];
static u64 aptChainloadTid;
static u8 aptChainloadDeliverArg[0x300];
static u32 aptChainloadDeliverArgSize = sizeof(aptChainloadDeliverArg);
static u8 aptChainloadHmac[0x20];
static u8 aptChainloadMediatype;
static u8 aptChainloadFlags;

static void aptEventHandler(void *arg);

// The following function can be overridden in order to log APT signals and notifications for debugging purposes
#ifdef LIBCTRU_APT_DEBUG
__attribute__((weak)) void _aptDebug(int a, int b) { }
#else
#define _aptDebug(a,b) ((void)0)
#endif

// APT<->DSP interaction functions (stubbed when not using DSP)
__attribute__((weak)) bool aptDspSleep(void) { return false; }
__attribute__((weak)) void aptDspWakeup(void) { }
__attribute__((weak)) void aptDspCancel(void) { }

static void aptCallHook(APT_HookType hookType)
{
	aptHookCookie* c;
	for (c = &aptFirstHook; c && c->callback; c = c->next)
		c->callback(hookType, c->param);
}

static bool aptIsReinit(void)
{
	return (envGetSystemRunFlags() & RUNFLAG_APTREINIT) != 0;
}

static bool aptIsCrippled(void)
{
	u32 flags = envGetSystemRunFlags();
	return (flags & RUNFLAG_APTWORKAROUND) && !(flags & RUNFLAG_APTREINIT);
}

static Result aptGetServiceHandle(Handle* aptuHandle)
{
	const char* serviceName = "APT:S";
	return srvGetServiceHandleDirect(aptuHandle, serviceName);
}

static inline int countPrmWords(u32 hdr)
{
	return 1 + (hdr&0x3F) + ((hdr>>6)&0x3F);
}

Result aptSendCommand(u32* aptcmdbuf)
{
	Handle aptuHandle;

	if (aptLockHandle) svcWaitSynchronization(aptLockHandle, U64_MAX);
	Result res = aptGetServiceHandle(&aptuHandle);
	if (R_SUCCEEDED(res))
	{
		u32* cmdbuf = getThreadCommandBuffer();
		memcpy(cmdbuf, aptcmdbuf, 4*countPrmWords(aptcmdbuf[0]));
		res = svcSendSyncRequest(aptuHandle);
		if (R_SUCCEEDED(res))
		{
			memcpy(aptcmdbuf, cmdbuf, 4*countPrmWords(cmdbuf[0]));
			res = aptcmdbuf[1];
		}
		svcCloseHandle(aptuHandle);
	}
	if (aptLockHandle) svcReleaseMutex(aptLockHandle);
	return res;
}

Result aptInit(void)
{
	_aptDebug(0x00, 0x00);

	Result ret=0;

	if (AtomicPostIncrement(&aptRefCount)) return 0;

	// Retrieve APT lock
	ret = APT_GetLockHandle(0x0, &aptLockHandle);
	if (R_FAILED(ret)) goto _fail;
	if (aptIsCrippled()) return 0;

	// Initialize APT
	// APT_AppletAttr attr = aptMakeAppletAttr(APTPOS_APP, false, false);
	APT_AppletAttr attr = 0x20000002;
	ret = APT_Initialize(envGetAptAppId(), attr, &aptEvents[APT_EVENT_NOTIFICATION_INDEX], &aptEvents[APT_EVENT_RESUME_INDEX]);
	if (R_FAILED(ret)) goto _fail2;

	// Initialize light events
	LightEvent_Init(&aptReceiveEvent, RESET_STICKY);
	LightEvent_Init(&aptSleepEvent, RESET_ONESHOT);

	// Create APT event handler thread
	aptEventHandlerThreadQuit = false;
	aptEventHandlerThread = threadCreate(aptEventHandler, 0x0, APT_HANDLER_STACKSIZE, 0x31, -2, true);
	if (!aptEventHandlerThread) goto _fail3;

	// By default allow sleep mode
	aptFlags = FLAG_ALLOWSLEEP;

	// Don't allow the homemenu, as we are the homemenu, it's ineffective
	aptFlags |= FLAG_ALLOWHOME;

	// Enable APT
	ret = APT_Enable(attr);
	if (R_FAILED(ret)) goto _fail3;

	// Wait for wakeup
	aptWaitForWakeUp(TR_ENABLE);

	// Special handling for aptReinit (aka hax 2.x):
	//   In certain cases when running under hax 2.x, we may receive a spurious
	//   second wakeup command. Therefore we must silently drop it in order to
	//   avoid keeping stale commands in APT's internal buffer.
	if (aptIsReinit())
		aptFlags |= FLAG_SPURIOUS;
	return 0;

_fail3:
	svcCloseHandle(aptEvents[APT_EVENT_NOTIFICATION_INDEX]);
	svcCloseHandle(aptEvents[APT_EVENT_RESUME_INDEX]);
_fail2:
	svcCloseHandle(aptLockHandle);
_fail:
	AtomicDecrement(&aptRefCount);
	return ret;
}

bool aptIsActive(void)
{
	return (aptFlags & FLAG_ACTIVE) != 0;
}

bool aptShouldClose(void)
{
	return (aptFlags & (FLAG_ORDERTOCLOSE|FLAG_CANCELLED)) != 0;
}

bool aptIsSleepAllowed(void)
{
	return (aptFlags & FLAG_ALLOWSLEEP) != 0;
}

void aptSetSleepAllowed(bool allowed)
{
	bool cur = aptIsSleepAllowed();
	if (allowed && !cur)
	{
		aptFlags |= FLAG_ALLOWSLEEP;
		APT_SleepIfShellClosed();
	}
	else if (!allowed && cur)
	{
		aptFlags &= ~FLAG_ALLOWSLEEP;
		APT_ReplySleepQuery(envGetAptAppId(), APTREPLY_REJECT);
	}
}

extern void (*__system_retAddr)(void);

static void aptExitProcess(void)
{
	APT_CloseApplication(NULL, 0, 0);
}

void aptExit(void)
{
	if (AtomicDecrement(&aptRefCount)) return;

	bool closeAptLock = true;
	bool doDirtyChainload = false;

	if (!aptIsCrippled())
	{
		bool doClose;
		if (aptShouldClose())
		{
			// The system instructed us to close, so do just that
			aptCallHook(APTHOOK_ONEXIT);
			doClose = true;
		}
		else if (aptIsReinit())
		{
			// The homebrew environment expects APT to be reinitializable, so unregister ourselves without closing
			APT_Finalize(envGetAptAppId());
			doClose = false;
		}
		else if (aptFlags & FLAG_CHAINLOAD)
		{
			// A chainload target is configured, so perform a jump to it
			// Doing this requires help from HOME menu, so ensure that it is running
			bool hmRegistered;
			if (R_SUCCEEDED(APT_IsRegistered(aptGetMenuAppID(), &hmRegistered)) && hmRegistered)
			{
				// Normal, sane chainload
				APT_PrepareToDoApplicationJump(aptChainloadFlags, aptChainloadTid, aptChainloadMediatype);
				APT_DoApplicationJump(aptChainloadDeliverArg, aptChainloadDeliverArgSize, aptChainloadHmac);
			}
			else
			{
				// XX: HOME menu doesn't exist, so we need to use a workaround provided by Luma3DS
				APT_Finalize(envGetAptAppId());
				doDirtyChainload = true;
			}

			// After a chainload has been applied, we don't need to manually close
			doClose = false;
			__system_retAddr = NULL;
		}
		else
		{
			// None of the other situations apply, so close anyway by default
			doClose = true;
		}

		// If needed, perform the APT application closing sequence
		if (doClose)
		{
			APT_PrepareToCloseApplication(true);

			// APT_CloseApplication kills us if we aren't signed up for srv closing notifications, so
			// defer APT_CloseApplication for as long as possible (TODO: actually use srv notif instead)
			__system_retAddr = aptExitProcess;
			closeAptLock = false;
			srvInit(); // Keep srv initialized
		}

		aptEventHandlerThreadQuit = true;
		svcSignalEvent(aptEvents[APT_EVENT_NOTIFICATION_INDEX]);
		threadJoin(aptEventHandlerThread, U64_MAX);
		int i;
		for (i = 0; i < 2; i ++)
			svcCloseHandle(aptEvents[i]);
	}

	if (closeAptLock)
		svcCloseHandle(aptLockHandle);

	if (doDirtyChainload)
	{
		// Provided by Luma3DS
		Handle notificationHandle = 0;
		Result res = 0;
		u32 notificationNumber = 0;
		srvEnableNotification(&notificationHandle);

		// Not needed, but official (sysmodule) code does this:
		srvSubscribe(0x100);

		// Make PM modify our run flags and ask us to terminate
		srvPublishToSubscriber(0x3000, 0);

		do
		{
			// Bail out after 3 seconds, we don't want to wait forever for this
			res = svcWaitSynchronization(notificationHandle, 3 * 1000 * 1000LL);
			res = res == 0 ? srvReceiveNotification(&notificationNumber) : res;
		} while(res == 0 && notificationNumber != 0x100);

		svcCloseHandle(notificationHandle);
	}
}

void aptEventHandler(void *arg)
{
	while (!aptEventHandlerThreadQuit)
	{
		s32 id = 0;
		svcWaitSynchronizationN(&id, aptEvents, 2, 0, U64_MAX);

		_aptDebug(0x55, 1);

		if (aptEventHandlerThreadQuit)
			break;

		// If the receive event is still signaled, sleep for a bit and retry
		if (LightEvent_TryWait(&aptReceiveEvent))
		{
			_aptDebug(0x55, 2);
			svcSleepThread(10000000); // 10ms
			svcSignalEvent(aptEvents[id]);
			continue;
		}

		// This is done by official sw, even though APT events are oneshot...
		svcClearEvent(aptEvents[id]);

		// Relay receive events to our light event
		if (id == APT_EVENT_RESUME_INDEX)
		{
			_aptDebug(0x55, 3);
			NS_APPID sender;
			APT_Command cmd;
			Result res = APT_GlanceParameter(envGetAptAppId(), aptParameters, sizeof(aptParameters), &sender, &cmd, NULL, NULL);
			if (R_FAILED(res))
				continue; // Official sw panics here - we instead swallow the (non-)event.

			_aptDebug(2, cmd); _aptDebug(22, sender);

			LightEvent_Signal(&aptReceiveEvent);

		} else if (id == APT_EVENT_NOTIFICATION_INDEX) {
			_aptDebug(0x55, 4);

			APT_Signal signal;
			Result res = APT_InquireNotification(envGetAptAppId(), &signal);

			if (R_FAILED(res))
				continue;

			if (aptSignalFunc)
				aptSignalFunc(signal);
		}
	}
}

// Somehow this function is being called when signaling aptReceiveEvent
static Result aptReceiveParameter(APT_Command* cmd, size_t* actualSize, Handle* handle)
{
	NS_APPID sender;
	size_t temp_actualSize;
	if (!actualSize) actualSize = &temp_actualSize;

	LightEvent_Wait(&aptReceiveEvent);
	LightEvent_Clear(&aptReceiveEvent);
	Result res = APT_ReceiveParameter(envGetAptAppId(), aptParameters, sizeof(aptParameters), &sender, cmd, actualSize, handle);
	_aptDebug(8, *cmd);
	if (R_SUCCEEDED(res) && *cmd == APTCMD_MESSAGE && aptMessageFunc)
		aptMessageFunc(aptMessageFuncData, sender, aptParameters, *actualSize);
	return res;
}


APT_Command aptGetCommand()
{
  APT_Command cmd;
  Result res = aptReceiveParameter(&cmd, NULL, NULL);

  _aptDebug(0x41, cmd);

  if (R_SUCCEEDED(res)) {
	return cmd;
  } else {
    return APTCMD_NONE;
  }
}

APT_Command aptWaitForWakeUp(APT_Transition transition)
{	
	_aptDebug(0x40, 0x00);
	APT_Command cmd;
	APT_NotifyToWait(envGetAptAppId());
	_aptDebug(0x40, 0x01);
	aptFlags &= ~FLAG_ACTIVE;
	if (transition != TR_ENABLE) {
		_aptDebug(0x40, 0x02);
		APT_SleepIfShellClosed();
	}

	_aptDebug(0x40, 0x03);

	for (;;)
	{
		_aptDebug(0x40, 0x04);
		Result res = aptReceiveParameter(&cmd, NULL, NULL);
		_aptDebug(0x41, cmd);
		if (R_SUCCEEDED(res)
			&& (cmd==APTCMD_WAKEUP || cmd==APTCMD_WAKEUP_PAUSE || cmd==APTCMD_WAKEUP_EXIT || cmd==APTCMD_WAKEUP_CANCEL
			|| cmd==APTCMD_WAKEUP_CANCELALL || cmd==APTCMD_WAKEUP_POWERBUTTON || cmd==APTCMD_WAKEUP_JUMPTOHOME
			|| cmd==APTCMD_WAKEUP_LAUNCHAPP)) {
				_aptDebug(0x40, 0x05);
				break;
		}	
			
	}
	aptFlags |= FLAG_ACTIVE;

	void __ctru_speedup_config();
	__ctru_speedup_config();

	if (transition != TR_CANCELLIB && cmd != APTCMD_WAKEUP_CANCEL && cmd != APTCMD_WAKEUP)
	{
		GSPGPU_AcquireRight(0);
		GSPGPU_RestoreVramSysArea();
		aptCallHook(APTHOOK_ONRESTORE);
	}

	if (cmd == APTCMD_WAKEUP_CANCEL || cmd == APTCMD_WAKEUP_CANCELALL)
	{
		aptDspCancel();
		if (cmd == APTCMD_WAKEUP_CANCEL) // for some reason, not for CANCELALL... is this a bug in official sw?
			aptFlags |= FLAG_CANCELLED;
	} else if (cmd != APTCMD_WAKEUP_LAUNCHAPP)
	{
		aptFlags &= ~FLAG_DSPWAKEUP;
		aptDspWakeup();
	}

	if (cmd != APTCMD_WAKEUP_JUMPTOHOME)
	{
		APT_UnlockTransition(0);
		APT_SleepIfShellClosed();
	} else
	{
		aptFlags |= FLAG_SHOULDHOME;
		APT_LockTransition(0x01, true);
	}

	return cmd;
}

void aptHandleSleep(void)
{
	if (!(aptFlags & FLAG_SHOULDSLEEP))
		return;

	_aptDebug(0x30, 1);

	aptFlags = (aptFlags &~ FLAG_SHOULDSLEEP) | FLAG_SLEEPING;
	aptCallHook(APTHOOK_ONSLEEP);
	APT_ReplySleepNotificationComplete(envGetAptAppId());
	LightEvent_Wait(&aptSleepEvent);
	aptFlags &= ~FLAG_SLEEPING;

	if (aptIsActive())
		GSPGPU_SetLcdForceBlack(0);
	aptCallHook(APTHOOK_ONWAKEUP);
}

bool aptMainLoop(void)
{
	aptHandleSleep();
	return !aptShouldClose();
}

void aptHook(aptHookCookie* cookie, aptHookFn callback, void* param)
{
	if (!callback) return;

	aptHookCookie* hook = &aptFirstHook;
	*cookie = *hook; // Structure copy.
	hook->next = cookie;
	hook->callback = callback;
	hook->param = param;
}

void aptUnhook(aptHookCookie* cookie)
{
	aptHookCookie* hook;
	for (hook = &aptFirstHook; hook; hook = hook->next)
	{
		if (hook->next == cookie)
		{
			*hook = *cookie; // Structure copy.
			break;
		}
	}
}

void aptSetMessageCallback(aptMessageCb callback, void* user)
{
	aptMessageFunc = callback;
	aptMessageFuncData = user;
}

void aptSetSignalCallback(aptSignalCb callback)
{
	aptSignalFunc = callback;
}

Result APT_GetLockHandle(u16 flags, Handle* lockHandle)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x1,1,0); // 0x10040
	cmdbuf[1]=flags;

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret))
		*lockHandle = cmdbuf[5];

	return ret;
}

Result APT_Initialize(NS_APPID appId, APT_AppletAttr attr, Handle* signalEvent, Handle* resumeEvent)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x2,2,0); // 0x20080
	cmdbuf[1]=appId;
	cmdbuf[2]=attr;

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret))
	{
		if(signalEvent) *signalEvent=cmdbuf[3];
		if(resumeEvent) *resumeEvent=cmdbuf[4];
	}

	return ret;
}

Result APT_Finalize(NS_APPID appId)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x4,1,0); // 0x40040
	cmdbuf[1]=appId;

	return aptSendCommand(cmdbuf);
}

Result APT_HardwareResetAsync(void)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x4E,0,0); // 0x4E0000

	return aptSendCommand(cmdbuf);
}

Result APT_Enable(APT_AppletAttr attr)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x3,1,0); // 0x30040
	cmdbuf[1]=attr;

	return aptSendCommand(cmdbuf);
}

Result APT_GetAppletManInfo(APT_AppletPos inpos, APT_AppletPos* outpos, NS_APPID* req_appid, NS_APPID* menu_appid, NS_APPID* active_appid)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x5,1,0); // 0x50040
	cmdbuf[1]=inpos;

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret))
	{
		if (outpos)       *outpos      =cmdbuf[2];
		if (req_appid)    *req_appid   =cmdbuf[3];
		if (menu_appid)   *menu_appid  =cmdbuf[4];
		if (active_appid) *active_appid=cmdbuf[5];
	}

	return ret;
}

Result APT_GetAppletInfo(NS_APPID appID, u64* pProgramID, u8* pMediaType, bool* pRegistered, bool* pLoadState, APT_AppletAttr* pAttributes)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x6,1,0); // 0x60040
	cmdbuf[1]=appID;

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret))
	{
		if (pProgramID)  *pProgramID =(u64)cmdbuf[2]|((u64)cmdbuf[3]<<32);
		if (pMediaType)  *pMediaType =cmdbuf[4];
		if (pRegistered) *pRegistered=cmdbuf[5] & 0xFF;
		if (pLoadState)  *pLoadState =cmdbuf[6] & 0xFF;
		if (pAttributes) *pAttributes=cmdbuf[7];
	}

	return ret;
}

Result APT_GetAppletProgramInfo(u32 id, u32 flags, u16 *titleversion)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x4D,2,0); // 0x4D0080
	cmdbuf[1]=id;
	cmdbuf[2]=flags;

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret))
		*titleversion=cmdbuf[2];

	return ret;
}

Result APT_GetProgramID(u64* pProgramID)
{
	u32 cmdbuf[16];
	cmdbuf[0] = IPC_MakeHeader(0x58,0,2); // 0x580002
	cmdbuf[1] = IPC_Desc_CurProcessId();

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret))
		*pProgramID=((u64)cmdbuf[3]<<32)|cmdbuf[2];

	return ret;
}

Result APT_IsRegistered(NS_APPID appID, bool* out)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x9,1,0); // 0x90040
	cmdbuf[1]=appID;

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret))
		*out=cmdbuf[2] & 0xFF;

	return ret;
}

Result APT_InquireNotification(u32 appID, APT_Signal* signalType)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0xB,1,0); // 0xB0040
	cmdbuf[1]=appID;

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret))
		*signalType=cmdbuf[2];

	return ret;
}

Result APT_PrepareToJumpToHomeMenu(void)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x2B,0,0); // 0x2B0000

	return aptSendCommand(cmdbuf);
}

Result APT_JumpToHomeMenu(const void* param, size_t paramSize, Handle handle)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x2C,1,4); // 0x2C0044
	cmdbuf[1]=paramSize;
	cmdbuf[2]=IPC_Desc_SharedHandles(1);
	cmdbuf[3]=handle;
	cmdbuf[4]=IPC_Desc_StaticBuffer(cmdbuf[1],0);
	cmdbuf[5]=(u32) param;

	return aptSendCommand(cmdbuf);
}

Result APT_PrepareToJumpToApplication(bool exiting)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x23,1,0); // 0x230040
	cmdbuf[1]=exiting;

	return aptSendCommand(cmdbuf);
}

Result APT_JumpToApplication(const void* param, size_t paramSize, Handle handle)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x24,1,4); // 0x240044
	cmdbuf[1]=paramSize;
	cmdbuf[2]=IPC_Desc_SharedHandles(1);
	cmdbuf[3]=handle;
	cmdbuf[4]=IPC_Desc_StaticBuffer(cmdbuf[1],0);
	cmdbuf[5]= (u32) param;

	return aptSendCommand(cmdbuf);
}

Result APT_SleepSystem(const PtmWakeEvents *wakeEvents)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x42, 2, 0);
	memcpy(&cmdbuf[1], wakeEvents, sizeof(PtmWakeEvents));

	return aptSendCommand(cmdbuf);
}

Result APT_NotifyToWait(NS_APPID appID)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x43,1,0); // 0x430040
	cmdbuf[1]=appID;

	return aptSendCommand(cmdbuf);
}

Result APT_AppletUtility(int id, void* out, size_t outSize, const void* in, size_t inSize)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x4B,3,2); // 0x4B00C2
	cmdbuf[1]=id;
	cmdbuf[2]=inSize;
	cmdbuf[3]=outSize;
	cmdbuf[4]=IPC_Desc_StaticBuffer(cmdbuf[2],1);
	cmdbuf[5]=(u32)in;

	u32 saved_threadstorage[2];
	u32* staticbufs = getThreadStaticBuffers();
	saved_threadstorage[0]=staticbufs[0];
	saved_threadstorage[1]=staticbufs[1];
	staticbufs[0]=IPC_Desc_StaticBuffer(cmdbuf[3],0);
	staticbufs[1]=(u32)out;

	Result ret = aptSendCommand(cmdbuf);
	staticbufs[0]=saved_threadstorage[0];
	staticbufs[1]=saved_threadstorage[1];

	return ret;
}

Result APT_SleepIfShellClosed(void)
{
	u8 out, in = 0;
	return APT_AppletUtility(4, &out, sizeof(out), &in, sizeof(in));
}

Result APT_LockTransition(u32 transition, bool flag)
{
	const struct
	{
		u32 transition;
		bool flag;
		u8 padding[3];
	} in = { transition, flag, {0} };
	u8 out;
	return APT_AppletUtility(5, &out, sizeof(out), &in, sizeof(in));
}

Result APT_TryLockTransition(u32 transition, bool* succeeded)
{
	return APT_AppletUtility(6, succeeded, sizeof(*succeeded), &transition, sizeof(transition));
}

Result APT_UnlockTransition(u32 transition)
{
	u8 out;
	return APT_AppletUtility(7, &out, sizeof(out), &transition, sizeof(transition));
}

Result APT_GlanceParameter(NS_APPID appID, void* buffer, size_t bufferSize, NS_APPID* sender, APT_Command* command, size_t* actualSize, Handle* parameter)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0xE,2,0); // 0xE0080
	cmdbuf[1]=appID;
	cmdbuf[2]=bufferSize;

	u32 saved_threadstorage[2];
	u32* staticbufs = getThreadStaticBuffers();
	saved_threadstorage[0]=staticbufs[0];
	saved_threadstorage[1]=staticbufs[1];
	staticbufs[0]=IPC_Desc_StaticBuffer(cmdbuf[2],0);
	staticbufs[1]=(u32)buffer;

	Result ret = aptSendCommand(cmdbuf);
	staticbufs[0]=saved_threadstorage[0];
	staticbufs[1]=saved_threadstorage[1];

	if (R_SUCCEEDED(ret))
	{
		if (sender)     *sender    =cmdbuf[2];
		if (command)    *command   =cmdbuf[3];
		if (actualSize) *actualSize=cmdbuf[4];
		if (parameter)  *parameter =cmdbuf[6];
		else if (cmdbuf[6]) svcCloseHandle(cmdbuf[6]);
	}

	return ret;
}

Result APT_ReceiveParameter(NS_APPID appID, void* buffer, size_t bufferSize, NS_APPID* sender, APT_Command* command, size_t* actualSize, Handle* parameter)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0xD,2,0); // 0xD0080
	cmdbuf[1]=appID;
	cmdbuf[2]=bufferSize;

	u32 saved_threadstorage[2];
	u32* staticbufs = getThreadStaticBuffers();
	saved_threadstorage[0]=staticbufs[0];
	saved_threadstorage[1]=staticbufs[1];
	staticbufs[0]=IPC_Desc_StaticBuffer(cmdbuf[2],0);
	staticbufs[1]=(u32)buffer;

	Result ret = aptSendCommand(cmdbuf);
	staticbufs[0]=saved_threadstorage[0];
	staticbufs[1]=saved_threadstorage[1];

	if (R_SUCCEEDED(ret))
	{
		if (sender)     *sender    =cmdbuf[2];
		if (command)    *command   =cmdbuf[3];
		if (actualSize) *actualSize=cmdbuf[4];
		if (parameter)  *parameter =cmdbuf[6];
		else if (cmdbuf[6]) svcCloseHandle(cmdbuf[6]);
	}

	return ret;
}

Result APT_SendParameter(NS_APPID source, NS_APPID dest, APT_Command command, const void* buffer, u32 bufferSize, Handle parameter)
{
	u32 cmdbuf[16];

	cmdbuf[0] = IPC_MakeHeader(0xC,4,4); // 0xC0104
	cmdbuf[1] = source;
	cmdbuf[2] = dest;
	cmdbuf[3] = command;
	cmdbuf[4] = bufferSize;

	cmdbuf[5] = IPC_Desc_SharedHandles(1);
	cmdbuf[6] = parameter;

	cmdbuf[7] = IPC_Desc_StaticBuffer(cmdbuf[4],0);
	cmdbuf[8] = (u32)buffer;

	return aptSendCommand(cmdbuf);
}

Result APT_CancelParameter(NS_APPID source, NS_APPID dest, bool* success)
{
	u32 cmdbuf[16];

	cmdbuf[0] = IPC_MakeHeader(0xF,4,0); // 0xF0100
	cmdbuf[1] = source != APPID_NONE;
	cmdbuf[2] = source;
	cmdbuf[3] = dest != APPID_NONE;
	cmdbuf[4] = dest;

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret) && success)
		*success = cmdbuf[2] & 0xFF;

	return ret;
}

Result APT_SendCaptureBufferInfo(const aptCaptureBufInfo* captureBuf)
{
	u32 cmdbuf[16];

	cmdbuf[0] = IPC_MakeHeader(0x40,1,2); // 0x400042
	cmdbuf[1] = sizeof(*captureBuf);
	cmdbuf[2] = IPC_Desc_StaticBuffer(cmdbuf[1],0);
	cmdbuf[3] = (u32)captureBuf;

	return aptSendCommand(cmdbuf);
}

Result APT_ReplySleepQuery(NS_APPID appID, APT_QueryReply reply)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x3E,2,0); // 0x3E0080
	cmdbuf[1]=appID;
	cmdbuf[2]=reply;

	return aptSendCommand(cmdbuf);
}

Result APT_ReplySleepNotificationComplete(NS_APPID appID)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x3F,1,0); // 0x3F0040
	cmdbuf[1]=appID;

	return aptSendCommand(cmdbuf);
}

Result APT_PrepareToCloseApplication(bool cancelPreload)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x22,1,0); // 0x220040
	cmdbuf[1]=cancelPreload;

	return aptSendCommand(cmdbuf);
}

Result APT_CloseApplication(const void* param, size_t paramSize, Handle handle)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x27,1,4); // 0x270044
	cmdbuf[1]=paramSize;
	cmdbuf[2]=IPC_Desc_SharedHandles(1);
	cmdbuf[3]=handle;
	cmdbuf[4]=IPC_Desc_StaticBuffer(cmdbuf[1],0);
	cmdbuf[5]= (u32) param;

	return aptSendCommand(cmdbuf);
}

//See http://3dbrew.org/APT:SetApplicationCpuTimeLimit
Result APT_SetAppCpuTimeLimit(u32 percent)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x4F,2,0); // 0x4F0080
	cmdbuf[1]=1;
	cmdbuf[2]=percent;

	return aptSendCommand(cmdbuf);
}

Result APT_GetAppCpuTimeLimit(u32 *percent)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x50,1,0); // 0x500040
	cmdbuf[1]=1;

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret))
		*percent=cmdbuf[2];

	return ret;
}

static Result APT_CheckNew3DS_System(bool* out)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x102,0,0); // 0x1020000

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret))
		*out = cmdbuf[2] & 0xFF;

	return ret;
}

Result APT_CheckNew3DS(bool* out)
{
	static bool flagInit, flagValue;
	if (!flagInit)
	{
		*out = false;
		Result ret = APT_CheckNew3DS_System(&flagValue);
		if (R_FAILED(ret)) return ret;
		flagInit = true;
	}

	*out = flagValue;
	return 0;
}

Result APT_PrepareToDoApplicationJump(u8 flags, u64 programID, u8 mediatype)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x31,4,0); // 0x310100
	cmdbuf[1]=flags;
	cmdbuf[2]=(u32)programID;
	cmdbuf[3]=(u32)(programID>>32);
	cmdbuf[4]=mediatype;

	return aptSendCommand(cmdbuf);
}

Result APT_DoApplicationJump(const void* param, size_t paramSize, const void* hmac)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x32,2,4); // 0x320084
	cmdbuf[1]=paramSize;
	cmdbuf[2]=hmac ? 0x20 : 0;
	cmdbuf[3]=IPC_Desc_StaticBuffer(cmdbuf[1],0);
	cmdbuf[4]=(u32)param;
	cmdbuf[5]=IPC_Desc_StaticBuffer(cmdbuf[2],2);
	cmdbuf[6]=(u32)hmac;

	return aptSendCommand(cmdbuf);
}

Result APT_PrepareToStartLibraryApplet(NS_APPID appID)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x18,1,0); // 0x180040
	cmdbuf[1]=appID;

	return aptSendCommand(cmdbuf);
}

Result APT_StartLibraryApplet(NS_APPID appID, const void* param, size_t paramSize, Handle handle)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x1E,2,4); // 0x1E0084
	cmdbuf[1]=appID;
	cmdbuf[2]=paramSize;
	cmdbuf[3]=IPC_Desc_SharedHandles(1);
	cmdbuf[4]=handle;
	cmdbuf[5]=IPC_Desc_StaticBuffer(cmdbuf[2],0);
	cmdbuf[6]=(u32)param;

	return aptSendCommand(cmdbuf);
}

Result APT_PrepareToStartSystemApplet(NS_APPID appID)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x19,1,0); // 0x190040
	cmdbuf[1]=appID;

	return aptSendCommand(cmdbuf);
}

Result APT_StartSystemApplet(NS_APPID appID, const void* param, size_t paramSize, Handle handle)
{
	u32 cmdbuf[16];
	cmdbuf[0] = IPC_MakeHeader(0x1F,2,4); // 0x001F0084
	cmdbuf[1] = appID;
	cmdbuf[2] = paramSize;
	cmdbuf[3] = IPC_Desc_SharedHandles(1);
	cmdbuf[4] = handle;
	cmdbuf[5] = IPC_Desc_StaticBuffer(cmdbuf[2],0);
	cmdbuf[6] = (u32)param;

	return aptSendCommand(cmdbuf);
}

Result APT_GetSharedFont(Handle* fontHandle, u32* mapAddr)
{
	u32 cmdbuf[16];
	cmdbuf[0] = IPC_MakeHeader(0x44,0,0); // 0x00440000

	Result ret = aptSendCommand(cmdbuf);
	if (R_SUCCEEDED(ret))
	{
		if(fontHandle) *fontHandle = cmdbuf[4];
		if(mapAddr)    *mapAddr    = cmdbuf[2];
	}

	return ret;
}

Result APT_ReceiveDeliverArg(void* param, size_t paramSize, void* hmac, u64* sender, bool* received)
{
	u32 cmdbuf[16];
	cmdbuf[0]=IPC_MakeHeader(0x35,2,0); // 0x350080
	cmdbuf[1]=paramSize;
	cmdbuf[2]=hmac ? 0x20 : 0;

	u32 saved_threadstorage[4];
	u32* staticbufs = getThreadStaticBuffers();
	saved_threadstorage[0]=staticbufs[0];
	saved_threadstorage[1]=staticbufs[1];
	saved_threadstorage[2]=staticbufs[2];
	saved_threadstorage[3]=staticbufs[3];
	staticbufs[0]=IPC_Desc_StaticBuffer(cmdbuf[1],0);
	staticbufs[1]=(u32)param;
	staticbufs[2]=IPC_Desc_StaticBuffer(cmdbuf[2],2);
	staticbufs[3]=(u32)hmac;

	Result ret = aptSendCommand(cmdbuf);
	staticbufs[0]=saved_threadstorage[0];
	staticbufs[1]=saved_threadstorage[1];
	staticbufs[2]=saved_threadstorage[2];
	staticbufs[3]=saved_threadstorage[3];

	if (R_SUCCEEDED(ret))
	{
		if (sender)     *sender    =cmdbuf[2] | ((u64)cmdbuf[3]<<32);
		if (received)   *received  =cmdbuf[4] & 0xFF;
	}

	return ret;
}

Result APT_PrepareToStartApplication(FS_ProgramInfo* programInfo, u8 launchFlags)
{
	// https://www.3dbrew.org/wiki/APT:PrepareToStartApplication

	u32 cmdbuf[16];

	cmdbuf[0]=0x00150140;
	memcpy(&cmdbuf[1], programInfo, sizeof(FS_ProgramInfo));
	cmdbuf[5] = launchFlags;

	return aptSendCommand(cmdbuf);
}

Result APT_StartApplication(u32 parameterSize, u32 hmacSize, u32 paused, void* parameter, void* hmac)
{
	// https://www.3dbrew.org/wiki/APT:StartApplication

	u32 cmdbuf[16];

	cmdbuf[0]=0x001B00C4;
	cmdbuf[1]=parameterSize;
	cmdbuf[2]=hmacSize;
	cmdbuf[3]=paused;

	// I checked which static buffer ids the real homemenu is using when performing this calls
	// 0x00 - For paramter
	// 0x02 - For hmac
	cmdbuf[4]=IPC_Desc_StaticBuffer(parameterSize,0x00);
	cmdbuf[5]=(u32)parameter;

	cmdbuf[6]=IPC_Desc_StaticBuffer(hmacSize,0x02);
	cmdbuf[7]=(u32)hmac;

	return aptSendCommand(cmdbuf);
}


Result APT_WakeupApplication(void)
{
	// https://www.3dbrew.org/wiki/APT:WakeupApplication

	u32 cmdbuf[16];

	cmdbuf[0]=0x001C0000;

	return aptSendCommand(cmdbuf);
}