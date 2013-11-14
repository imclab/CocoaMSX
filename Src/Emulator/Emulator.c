/*****************************************************************************
** $Source: /cygdrive/d/Private/_SVNROOT/bluemsx/blueMSX/Src/Emulator/Emulator.c,v $
**
** $Revision: 73 $
**
** $Date: 2012-10-19 17:10:16 -0700 (Fri, 19 Oct 2012) $
**
** More info: http://www.bluemsx.com
**
** Copyright (C) 2003-2006 Daniel Vik
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
******************************************************************************
*/
#include "Emulator.h"
#include "MsxTypes.h"
#include "Debugger.h"
#include "Board.h"
#include "FileHistory.h"
#include "Switches.h"
#include "Led.h"
#include "Machine.h"
#include "InputEvent.h"

#include "ArchThread.h"
#include "ArchEvent.h"
#include "ArchTimer.h"
#include "ArchSound.h"
#include "ArchMidi.h"

#include "JoystickPort.h"
#include "ArchInput.h"
#include "ArchDialog.h"
#include "ArchNotifications.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

//#define ENABLE_LOG

static int WaitForSync(Emulator *emulator, int maxSpeed, int breakpointHit);

struct Emulator {
    void*  emuThread;
#ifndef WII
    void*  emuSyncEvent;
#endif
    void*  emuStartEvent;
#ifndef WII
    void*  emuTimer;
#endif
    int    emuExitFlag;
    UInt32 emuSysTime;
    UInt32 emuFrequency;
    int    emuMaxSpeed;
    int    emuPlayReverse;
    int    emuMaxEmuSpeed; // Max speed issued by emulation
    char   emuStateName[512];
    volatile int      emuSuspendFlag;
    volatile EmuState emuState;
    volatile int      emuSingleStep;
    Properties* properties;
    Mixer* mixer;
    BoardDeviceInfo deviceInfo;
    Machine* machine;
    int lastScreenMode;
    
    int emuFrameskipCounter;
    
    UInt32 emuTimeIdle;
    UInt32 emuTimeTotal;
    UInt32 emuTimeOverflow;
    UInt32 emuUsageCurrent;
    UInt32 emuCpuSpeed;
    UInt32 emuCpuUsage;
    int    enableSynchronousUpdate;
    
#ifdef ENABLE_LOG
    UInt32 logentry[LOG_SIZE];
    int    logindex;
    int    logwrapped;
#endif
};

#ifdef ENABLE_LOG

#define LOG_SIZE (10 * 1000000)

void dolog(int slot, int sslot, int wr, UInt16 addr, UInt8 val)
{
    logentry[logindex++] = (slot << 26) | (sslot << 24) | ((UInt32)val << 16) | addr | (wr ? (1 << 31) : 0);
    if (logindex == LOG_SIZE) {
        logindex = 0;
        logwrapped++;
    }
}

void clearlog()
{
    logwrapped = 0;
    logindex = 0;
}

void savelog()
{
    int totalSize = LOG_SIZE;
    int lastPct = -1;
    int cnt = 0;
    FILE * f = fopen("bluemsxlog.txt", "w+");
    int i = 0;
    if (logwrapped == 0 && logindex == 0) {
        return;
    }

    if (logwrapped) {
        i = logindex;
    }
    else {
        totalSize = logindex;
    }

    printf("Saving log for slot 1\n");

    do {
        UInt32 v = logentry[i];
        int newPct = ++cnt * 100 / totalSize;
        char rw = (v >> 31) ? 'W' : 'R';

        if (newPct != lastPct) {
            printf("\r%d%%",newPct);
            lastPct = newPct;
        }
        fprintf(f, "%c(%d:%d) %.4x: %.2x\n", rw, (v>>26)&3, (v>>24)&3,v & 0xffff, (v >> 16) & 0xff);

        if (++i == LOG_SIZE) {
            i = 0;
        }
    } while (i != logindex);
    printf("\n");
    fclose(f);
}
#else
#define clearlog()
#define savelog()
#endif

Emulator * emulatorCreate(Properties *properties, Mixer *mixer) {
    Emulator *emulator = (Emulator *)malloc(sizeof(Emulator));
    
    emulator->emuSysTime = 0;
    emulator->emuFrequency = 3579545;
    emulator->emuMaxSpeed = 0;
    emulator->emuPlayReverse = 0;
    emulator->emuMaxEmuSpeed = 0;
    emulator->emuState = EMU_STOPPED;
    emulator->emuSingleStep = 0;
    
    emulator->emuFrameskipCounter = 0;
    
    emulator->emuTimeIdle = 0;
    emulator->emuTimeTotal = 1;
    emulator->emuTimeOverflow = 0;
    emulator->emuUsageCurrent = 0;
    emulator->emuCpuSpeed = 0;
    emulator->emuCpuUsage = 0;
    emulator->enableSynchronousUpdate = 1;
    
    emulator->properties = properties;
    emulator->mixer = mixer;
    
    return emulator;
}

static void emuCalcCpuUsage(Emulator *emulator) {
    static UInt32 oldSysTime = 0;
    static UInt32 cnt = 0;
    UInt32 newSysTime;
    UInt32 emuTimeAverage;

    if (emulator->emuTimeTotal < 10) {
        return;
    }
    newSysTime = archGetSystemUpTime(1000);
    emuTimeAverage = 100 * (emulator->emuTimeTotal - emulator->emuTimeIdle) / (emulator->emuTimeTotal / 10);

    emulator->emuTimeOverflow = emuTimeAverage > 940;

    if ((cnt++ & 0x1f) == 0) {
        UInt32 usageAverage = emulator->emuUsageCurrent * 100 / (newSysTime - oldSysTime) * emulator->emuFrequency / 3579545;
        if (usageAverage > 98 && usageAverage < 102) {
            usageAverage = 100;
        }

        if (usageAverage >= 10000) {
            usageAverage = 0;
        }

        emulator->emuCpuSpeed = usageAverage;
        emulator->emuCpuUsage = emuTimeAverage;
    }

    oldSysTime      = newSysTime;
    emulator->emuUsageCurrent = 0;
    emulator->emuTimeIdle     = 0;
    emulator->emuTimeTotal    = 1;
}

static int emulatorUseSynchronousUpdate(Emulator *emulator)
{
    if (emulator->properties->emulation.syncMethod == P_EMU_SYNCIGNORE) {
        return emulator->properties->emulation.syncMethod;
    }

    if (emulator->properties->emulation.speed == 50 &&
        emulator->enableSynchronousUpdate &&
        emulatorGetMaxSpeed(emulator) == 0)
    {
        return emulator->properties->emulation.syncMethod;
    }
    return P_EMU_SYNCAUTO;
}


UInt32 emulatorGetCpuSpeed(const Emulator *emulator) {
    return emulator->emuCpuSpeed;
}

UInt32 emulatorGetCpuUsage(const Emulator *emulator) {
    return emulator->emuCpuUsage;
}

void emulatorEnableSynchronousUpdate(Emulator *emulator, int enable)
{
    emulator->enableSynchronousUpdate = enable;
}

Properties * emulatorGetProperties(const Emulator *emulator)
{
    return emulator->properties;
}

Mixer * emulatorGetMixer(const Emulator *emulator)
{
    return emulator->mixer;
}

void emulatorSetProperties(Emulator *emulator, Properties* properties)
{
    emulator->properties = properties;
}

void emulatorSetMixer(Emulator *emulator, Mixer *mixer)
{
    emulator->mixer = mixer;
}

void emulatorDestroy(Emulator *emulator)
{
    free(emulator);
}

EmuState emulatorGetState(const Emulator *emulator) {
    return emulator->emuState;
}

void emulatorSetState(Emulator *emulator, EmuState state) {
    if (state == EMU_RUNNING) {
        archSoundResume();
        archMidiEnable(1);
    }
    else {
        archSoundSuspend();
        archMidiEnable(0);
    }
    if (state == EMU_STEP) {
        state = EMU_RUNNING;
        emulator->emuSingleStep = 1;
    }
    emulator->emuState = state;
}


int emulatorGetSyncPeriod(const Emulator *emulator) {
#ifdef NO_HIRES_TIMERS
    return 10;
#else
    return emulator->properties->emulation.syncMethod == P_EMU_SYNCAUTO ||
           emulator->properties->emulation.syncMethod == P_EMU_SYNCNONE ? 2 : 1;
#endif
}

#ifndef WII
static int timerCallback(Emulator *emulator) {
    if (emulator == NULL || emulator->properties == NULL) {
        return 1;
    }
    else {
        Properties *properties = emulator->properties;
        static UInt32 frameCount = 0;
        static UInt32 oldSysTime = 0;
        static UInt32 refreshRate = 50;
        UInt32 framePeriod = (properties->video.frameSkip + 1) * 1000;
//        UInt32 syncPeriod = emulatorGetSyncPeriod();
        UInt32 sysTime = archGetSystemUpTime(1000);
        UInt32 diffTime = sysTime - oldSysTime;
        int syncMethod = emulatorUseSynchronousUpdate(emulator);

        if (diffTime == 0) {
            return 0;
        }

        oldSysTime = sysTime;

        // Update display
        frameCount += refreshRate * diffTime;
        if (frameCount >= framePeriod) {
            frameCount %= framePeriod;
            if (emulator->emuState == EMU_RUNNING) {
                refreshRate = boardGetRefreshRate();

                if (syncMethod == P_EMU_SYNCAUTO || syncMethod == P_EMU_SYNCNONE) {
                    archUpdateEmuDisplay(0);
                }
            }
        }

        if (syncMethod == P_EMU_SYNCTOVBLANKASYNC) {
            archUpdateEmuDisplay(syncMethod);
        }

        // Update emulation
        archEventSet(emulator->emuSyncEvent);
    }

    return 1;
}
#endif

static void getDeviceInfo(Emulator *emulator)
{
    BoardDeviceInfo deviceInfo = emulator->deviceInfo;
    int i;

    for (i = 0; i < PROP_MAX_CARTS; i++) {
        strcpy(emulator->properties->media.carts[i].fileName, deviceInfo.carts[i].name);
        strcpy(emulator->properties->media.carts[i].fileNameInZip, deviceInfo.carts[i].inZipName);
        // Don't save rom type
        // properties->media.carts[i].type = deviceInfo->carts[i].type;
        updateExtendedRomName(i, emulator->properties->media.carts[i].fileName,
                              emulator->properties->media.carts[i].fileNameInZip);
    }

    for (i = 0; i < PROP_MAX_DISKS; i++) {
        strcpy(emulator->properties->media.disks[i].fileName, deviceInfo.disks[i].name);
        strcpy(emulator->properties->media.disks[i].fileNameInZip, deviceInfo.disks[i].inZipName);
        updateExtendedDiskName(i, emulator->properties->media.disks[i].fileName,
                               emulator->properties->media.disks[i].fileNameInZip);
    }

    for (i = 0; i < PROP_MAX_TAPES; i++) {
        strcpy(emulator->properties->media.tapes[i].fileName, deviceInfo.tapes[i].name);
        strcpy(emulator->properties->media.tapes[i].fileNameInZip, deviceInfo.tapes[i].inZipName);
        updateExtendedCasName(i, emulator->properties->media.tapes[i].fileName,
                              emulator->properties->media.tapes[i].fileNameInZip);
    }

    emulator->properties->emulation.vdpSyncMode = deviceInfo.video.vdpSyncMode;
}

static void setDeviceInfo(Emulator *emulator, BoardDeviceInfo* deviceInfo)
{
    int i;

    for (i = 0; i < PROP_MAX_CARTS; i++) {
        deviceInfo->carts[i].inserted =  strlen(emulator->properties->media.carts[i].fileName);
        deviceInfo->carts[i].type = emulator->properties->media.carts[i].type;
        strcpy(deviceInfo->carts[i].name, emulator->properties->media.carts[i].fileName);
        strcpy(deviceInfo->carts[i].inZipName, emulator->properties->media.carts[i].fileNameInZip);
    }

    for (i = 0; i < PROP_MAX_DISKS; i++) {
        deviceInfo->disks[i].inserted = strlen(emulator->properties->media.disks[i].fileName);
        strcpy(deviceInfo->disks[i].name, emulator->properties->media.disks[i].fileName);
        strcpy(deviceInfo->disks[i].inZipName, emulator->properties->media.disks[i].fileNameInZip);
    }

    for (i = 0; i < PROP_MAX_TAPES; i++) {
        deviceInfo->tapes[i].inserted =  strlen(emulator->properties->media.tapes[i].fileName);
        strcpy(deviceInfo->tapes[i].name, emulator->properties->media.tapes[i].fileName);
        strcpy(deviceInfo->tapes[i].inZipName, emulator->properties->media.tapes[i].fileNameInZip);
    }

    deviceInfo->video.vdpSyncMode = emulator->properties->emulation.vdpSyncMode;
}

static int emulationStartFailure = 0;

static void emulatorPauseCb(Emulator *emulator)
{
    emulatorSetState(emulator, EMU_PAUSED);
    debuggerNotifyEmulatorPause();
}

static void emulatorThread(Emulator *emulator) {
    Properties *properties = emulator->properties;
    int frequency;
    int success = 0;
    int reversePeriod = 0;
    int reverseBufferCnt = 0;

    emulatorSetFrequency(emulator, properties->emulation.speed, &frequency);

    switchSetFront(properties->emulation.frontSwitch);
    switchSetPause(properties->emulation.pauseSwitch);
    switchSetAudio(properties->emulation.audioSwitch);

    if (properties->emulation.reverseEnable && properties->emulation.reverseMaxTime > 0) {
        reversePeriod = 150;
        reverseBufferCnt = properties->emulation.reverseMaxTime * 1000 / reversePeriod;
    }
    success = boardRun(emulator,
                       emulator->machine,
                       emulator->deviceInfo,
                       *emulator->emuStateName ? emulator->emuStateName : NULL,
                       frequency, 
                       reversePeriod,
                       reverseBufferCnt,
                       WaitForSync);

    ledSetAll(0);
    emulator->emuState = EMU_STOPPED;

#ifndef WII
    archTimerDestroy(emulator->emuTimer);
#endif

    if (!success) {
        emulationStartFailure = 1;
    }

    archEventSet(emulator->emuStartEvent);
}

//extern int xxxx;

void emulatorStart(Emulator *emulator, const char* stateName) {
        dbgEnable();

    archEmulationStartNotification();
//xxxx = 0;
    emulatorResume(emulator);

    emulator->emuExitFlag = 0;

    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_MOONSOUND, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_YAMAHA_SFG, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_MSXAUDIO, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_MSXMUSIC, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_SCC, 1);


    emulator->properties->emulation.pauseSwitch = 0;
    switchSetPause(emulator->properties->emulation.pauseSwitch);

    emulator->machine = machineCreate(emulator->properties->emulation.machineName);

    if (emulator->machine == NULL) {
        archShowStartEmuFailDialog();
        archEmulationStopNotification();
        emulator->emuState = EMU_STOPPED;
        archEmulationStartFailure();
        return;
    }

    boardSetMachine(emulator->machine);

#ifndef NO_TIMERS
#ifndef WII
    emulator->emuSyncEvent  = archEventCreate(0);
#endif
    emulator->emuStartEvent = archEventCreate(0);
#ifndef WII
    emulator->emuTimer = archCreateTimer(emulator, timerCallback);
#endif
#endif

    setDeviceInfo(emulator, &emulator->deviceInfo);

    inputEventReset();

    archSoundResume();
    archMidiEnable(1);

    emulator->emuState = EMU_PAUSED;
    emulationStartFailure = 0;
    strncpy(emulator->emuStateName, stateName ? stateName : "",
            sizeof(emulator->emuStateName) - 1);

    clearlog();

#ifdef SINGLE_THREADED
    emulator->emuState = EMU_RUNNING;
    emulatorThread(emulator);

    if (emulationStartFailure) {
        archEmulationStopNotification();
        emulator->emuState = EMU_STOPPED;
        archEmulationStartFailure();
    }
#else
    emulator->emuThread = archThreadCreate(emulatorThread, emulator, THREAD_PRIO_HIGH);

    archEventWait(emulator->emuStartEvent, 3000);

    if (emulationStartFailure) {
        archEmulationStopNotification();
        emulator->emuState = EMU_STOPPED;
        archEmulationStartFailure();
    }
    if (emulator->emuState != EMU_STOPPED) {
        getDeviceInfo(emulator);

        boardSetYm2413Oversampling(emulator->properties->sound.chip.ym2413Oversampling);
        boardSetY8950Oversampling(emulator->properties->sound.chip.y8950Oversampling);
        boardSetMoonsoundOversampling(emulator->properties->sound.chip.moonsoundOversampling);

        strcpy(emulator->properties->emulation.machineName, emulator->machine->name);

        debuggerNotifyEmulatorStart();

        emulator->emuState = EMU_RUNNING;
    }
#endif
}

void emulatorStop(Emulator *emulator) {
    if (emulator->emuState == EMU_STOPPED) {
        return;
    }

    debuggerNotifyEmulatorStop();

    emulator->emuState = EMU_STOPPED;

    do {
        archThreadSleep(10);
    } while (!emulator->emuSuspendFlag);

    emulator->emuExitFlag = 1;
#ifndef WII
    archEventSet(emulator->emuSyncEvent);
#endif
    archSoundSuspend();
    archThreadJoin(emulator->emuThread, 3000);
    archMidiEnable(0);
    machineDestroy(emulator->machine);
    archThreadDestroy(emulator->emuThread);
#ifndef WII
    archEventDestroy(emulator->emuSyncEvent);
#endif
    archEventDestroy(emulator->emuStartEvent);

    // Reset active indicators in mixer
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_MOONSOUND, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_YAMAHA_SFG, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_MSXAUDIO, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_MSXMUSIC, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_SCC, 1);

    archEmulationStopNotification();

    dbgDisable();
    dbgPrint();
    savelog();
}



void emulatorSetFrequency(Emulator *emulator, int logFrequency, int* frequency) {
    emulator->emuFrequency = (int)(3579545 * pow(2.0, (logFrequency - 50) / 15.0515));

    if (frequency != NULL) {
        *frequency  = emulator->emuFrequency;
    }

    boardSetFrequency(emulator->emuFrequency);
}

void emulatorSuspend(Emulator *emulator) {
    if (emulator->emuState == EMU_RUNNING) {
        emulator->emuState = EMU_SUSPENDED;
        do {
            archThreadSleep(10);
        } while (!emulator->emuSuspendFlag);
        archSoundSuspend();
        archMidiEnable(0);
    }
}

void emulatorResume(Emulator *emulator) {
    if (emulator->emuState == EMU_SUSPENDED) {
        emulator->emuSysTime = 0;

        archSoundResume();
        archMidiEnable(1);
        emulator->emuState = EMU_RUNNING;
        archUpdateEmuDisplay(0);
    }
}

int emulatorGetCurrentScreenMode(const Emulator *emulator)
{
    return emulator->lastScreenMode;
}

void emulatorRestart(Emulator *emulator) {
    Machine* machine = machineCreate(emulator->properties->emulation.machineName);

    emulatorStop(emulator);
    if (machine != NULL) {
        boardSetMachine(machine);
        machineDestroy(machine);
    }
}

void emulatorRestartSound(Emulator *emulator) {
    emulatorSuspend(emulator);
    archSoundDestroy();
    archSoundCreate(emulator->mixer, 44100,
                    emulator->properties->sound.bufSize,
                    emulator->properties->sound.stereo ? 2 : 1);
    emulatorResume(emulator);
}

int emulatorGetCpuOverflow(Emulator *emulator) {
    int overflow = emulator->emuTimeOverflow;
    emulator->emuTimeOverflow = 0;
    
    return overflow;
}

void emulatorSetMaxSpeed(Emulator *emulator, int enable) {
    emulator->emuMaxSpeed = enable;
}

int  emulatorGetMaxSpeed(const Emulator *emulator) {
    return emulator->emuMaxSpeed;
}

void emulatorSetPlayReverse(Emulator *emulator, int enable)
{
    if (enable) {
        archSoundSuspend();
    }
    else {
        archSoundResume();
    }
    emulator->emuPlayReverse = enable;
}

int  emulatorGetPlayReverse(const Emulator *emulator)
{
    return emulator->emuPlayReverse;
}

void emulatorResetMixer(Emulator *emulator) {
    // Reset active indicators in mixer
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_MOONSOUND, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_YAMAHA_SFG, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_MSXAUDIO, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_MSXMUSIC, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_SCC, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_PCM, 1);
    mixerIsChannelTypeActive(emulator->mixer, MIXER_CHANNEL_IO, 1);
}

int emulatorSyncScreen(Emulator *emulator)
{
    int rv = 0;
    emulator->emuFrameskipCounter--;
    if (emulator->emuFrameskipCounter < 0) {
        rv = archUpdateEmuDisplay(emulator->properties->emulation.syncMethod);
        if (rv) {
            emulator->emuFrameskipCounter = emulator->properties->video.frameSkip;
        }
    }
    return rv;
}


void RefreshScreen(Emulator *emulator, int screenMode) {
    emulator->lastScreenMode = screenMode;

    if (emulatorUseSynchronousUpdate(emulator) == P_EMU_SYNCFRAMES && emulator->emuState == EMU_RUNNING) {
        emulatorSyncScreen(emulator);
    }
}

#ifndef NO_TIMERS

#ifdef WII

static int WaitForSync(Emulator *emulator, int maxSpeed, int breakpointHit)
{
    UInt32 diffTime;

    emulator->emuMaxEmuSpeed = maxSpeed;

    emulator->emuSuspendFlag = 1;

    archPollInput();

    if (emulator->emuState != EMU_RUNNING) {
        archEventSet(emulator->emuStartEvent);
        archThreadSleep(100);
        emulator->emuSuspendFlag = 0;
        return emulator->emuExitFlag ? -1 : 0;
    }

    emulator->emuSuspendFlag = 0;

    if (emulator->emuSingleStep) {
        diffTime = 0;
    }else{
        diffTime = 20;
    }

    if (emulator->emuMaxSpeed || emulator->emuMaxEmuSpeed) {
        diffTime *= 10;
    }

    return emulator->emuExitFlag ? -1 : diffTime;
}

#else

int WaitReverse(Emulator *emulator)
{
    boardEnableSnapshots(0);

    for (;;) {
        UInt32 sysTime = archGetSystemUpTime(1000);
        UInt32 diffTime = sysTime - emulator->emuSysTime;
        if (diffTime >= 50) {
            emulator->emuSysTime = sysTime;
            break;
        }
        archEventWait(emulator->emuSyncEvent, -1);
    }

    boardRewind();

    return -60;
}

static int WaitForSync(Emulator *emulator, int maxSpeed, int breakpointHit) {
    UInt32 li1;
    UInt32 li2;
    static UInt32 tmp = 0;
    static UInt32 cnt = 0;
    UInt32 sysTime;
    UInt32 diffTime;
    UInt32 syncPeriod;
    static int overflowCount = 0;
    static UInt32 kbdPollCnt = 0;

    if (emulator->emuPlayReverse && emulator->properties->emulation.reverseEnable) {
        return WaitReverse(emulator);
    }

//    boardEnableSnapshots(1); // AK TODO

    emulator->emuMaxEmuSpeed = maxSpeed;

    syncPeriod = emulatorGetSyncPeriod(emulator);
    li1 = archGetHiresTimer();

    emulator->emuSuspendFlag = 1;

    if (emulator->emuSingleStep) {
        debuggerNotifyEmulatorPause();
        emulator->emuSingleStep = 0;
        emulator->emuState = EMU_PAUSED;
        archSoundSuspend();
        archMidiEnable(0);
    }

    if (breakpointHit) {
        debuggerNotifyEmulatorPause();
        emulator->emuState = EMU_PAUSED;
        archSoundSuspend();
        archMidiEnable(0);
    }

    if (emulator->emuState != EMU_RUNNING) {
        archEventSet(emulator->emuStartEvent);
        emulator->emuSysTime = 0;
    }

#ifdef SINGLE_THREADED
    emulator->emuExitFlag |= archPollEvent();
#endif

    if (((++kbdPollCnt & 0x03) >> 1) == 0) {
       archPollInput();
    }

    if (emulatorUseSynchronousUpdate(emulator) == P_EMU_SYNCTOVBLANK) {
        overflowCount += emulatorSyncScreen(emulator) ? 0 : 1;
        while ((!emulator->emuExitFlag && emulator->emuState != EMU_RUNNING) || overflowCount > 0) {
            archEventWait(emulator->emuSyncEvent, -1);
#ifdef NO_TIMERS
            while (timerCallback(emulator) == 0) emuExitFlag |= archPollEvent();
#endif
            overflowCount--;
        }
    }
    else {
        do {
#ifdef NO_TIMERS
            while (timerCallback(emulator) == 0) emuExitFlag |= archPollEvent();
#endif
            if (!emulator->emuExitFlag)
                archEventWait(emulator->emuSyncEvent, -1);
            
            if (((emulator->emuMaxSpeed || emulator->emuMaxEmuSpeed)
                 && !emulator->emuExitFlag) || overflowCount > 0) {
#ifdef NO_TIMERS
                while (timerCallback(emulator) == 0) emuExitFlag |= archPollEvent();
#endif
                if (!emulator->emuExitFlag)
                    archEventWait(emulator->emuSyncEvent, -1);
            }
            overflowCount = 0;
        } while (!emulator->emuExitFlag && emulator->emuState != EMU_RUNNING);
    }

    emulator->emuSuspendFlag = 0;
    li2 = archGetHiresTimer();

    emulator->emuTimeIdle  += li2 - li1;
    emulator->emuTimeTotal += li2 - tmp;
    tmp = li2;

    sysTime = archGetSystemUpTime(1000);
    diffTime = sysTime - emulator->emuSysTime;
    emulator->emuSysTime = sysTime;

    if (emulator->emuSingleStep) {
        diffTime = 0;
    }

    if ((++cnt & 0x0f) == 0) {
        emuCalcCpuUsage(emulator);
    }

    overflowCount = emulatorGetCpuOverflow(emulator) ? 1 : 0;
#ifdef NO_HIRES_TIMERS
    if (diffTime > 50U) {
        overflowCount = 1;
        diffTime = 0;
    }
#else
    if (diffTime > 100U) {
        overflowCount = 1;
        diffTime = 0;
    }
#endif
    if (emulator->emuMaxSpeed || emulator->emuMaxEmuSpeed) {
        diffTime *= 10;
        if (diffTime > 20 * syncPeriod) {
            diffTime =  20 * syncPeriod;
        }
    }

    emulator->emuUsageCurrent += diffTime;

    return emulator->emuExitFlag ? -99 : diffTime;
}
#endif

#else
#include <windows.h>

UInt32 getHiresTimer() {
    static LONGLONG hfFrequency = 0;
    LARGE_INTEGER li;

    if (!hfFrequency) {
        if (QueryPerformanceFrequency(&li)) {
            hfFrequency = li.QuadPart;
        }
        else {
            return 0;
        }
    }

    QueryPerformanceCounter(&li);

    return (DWORD)(li.QuadPart * 1000000 / hfFrequency);
}

static UInt32 busy, total, oldTime;

static int WaitForSync(Emulator *emulator, int maxSpeed, int breakpointHit) {
    emulator->emuSuspendFlag = 1;

    busy += getHiresTimer() - oldTime;

    emulator->emuExitFlag |= archPollEvent();

    archPollInput();

    do {
        for (;;) {
            UInt32 sysTime = archGetSystemUpTime(1000);
            UInt32 diffTime = sysTime - emulator->emuSysTime;
            emulator->emuExitFlag |= archPollEvent();
            if (diffTime < 10) {
                continue;
            }
            emulator->emuSysTime += 10;
            if (diffTime > 30) {
                emulator->emuSysTime = sysTime;
            }
            break;
        }
    } while (!emulator->emuExitFlag && emulator->emuState != EMU_RUNNING);


    emulator->emuSuspendFlag = 0;

    total += getHiresTimer() - oldTime;
    oldTime = getHiresTimer();
#if 0
    if (total >= 1000000) {
        UInt32 pct = 10000 * busy / total;
        printf("CPU Usage = %d.%d%%\n", pct / 100, pct % 100);
        total = 0;
        busy = 0;
    }
#endif

    return emulator->emuExitFlag ? -1 : 10;
}

#endif

