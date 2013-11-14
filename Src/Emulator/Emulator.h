/*****************************************************************************
** $Source: /cygdrive/d/Private/_SVNROOT/bluemsx/blueMSX/Src/Emulator/Emulator.h,v $
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
#ifndef EMULATOR_H
#define EMULATOR_H

#include "MsxTypes.h"
#include "Properties.h"
#include "AudioMixer.h"

typedef enum { EMU_RUNNING, EMU_PAUSED, EMU_STOPPED, EMU_SUSPENDED, EMU_STEP } EmuState;

typedef struct Emulator Emulator;

Emulator * emulatorCreate(Properties *properties, Mixer *mixer);
Properties * emulatorGetProperties(const Emulator *emulator);
Mixer * emulatorGetMixer(const Emulator *emulator);
void emulatorSetProperties(Emulator *emulator, Properties* properties);
void emulatorSetMixer(Emulator *emulator, Mixer *mixer);
void emulatorDestroy(Emulator *emulator);

void emulatorEnableSynchronousUpdate(Emulator *emulator, int enable);

void emulatorSetFrequency(Emulator *emulator, int logFrequency, int* frequency);
void emulatorRestartSound(Emulator *emulator);
void emulatorSuspend(Emulator *emulator);
void emulatorResume(Emulator *emulator);
void emulatorDoResume(Emulator *emulator);
void emulatorRestart(Emulator *emulator);
void emulatorStart(Emulator *emulator, const char* stateName);
void emulatorStop(Emulator *emulator);
void emulatorSetMaxSpeed(Emulator *emulator, int enable);
int  emulatorGetMaxSpeed(const Emulator *emulator);
void emulatorSetPlayReverse(Emulator *emulator, int enable);
int  emulatorGetPlayReverse(const Emulator *emulator);
int emulatorGetCpuOverflow(Emulator *emulator);
int emulatorGetSyncPeriod(const Emulator *emulator);
EmuState emulatorGetState(const Emulator *emulator);
void emulatorSetState(Emulator *emulator, EmuState state);
UInt32 emulatorGetCpuSpeed(const Emulator *emulator);
UInt32 emulatorGetCpuUsage(const Emulator *emulator);
void emulatorResetMixer(Emulator *emulator);
int emulatorGetCurrentScreenMode(const Emulator *emulator);

#endif

