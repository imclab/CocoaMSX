/*****************************************************************************
 **
 ** CocoaMSX: MSX Emulator for Mac OS X
 ** http://www.cocoamsx.com
 ** Copyright (C) 2012-2013 Akop Karapetyan
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
#import "CMThread.h"

#include <pthread.h>
#include "Emulator.h"

#pragma mark - CMThreadArg

// AK: Why not use NSThread? NSThread isn't joinable

@interface CMThreadArg : NSObject
{
@public
    CMThreadEntryPoint entryPoint;
    Emulator *emulator;
}
@end

@implementation CMThreadArg

@end

#pragma mark - CMThread

@interface CMThread ()

- (void)start;
- (void)join;
+ (void)sleepMilliseconds:(NSInteger)ms;

@property (nonatomic, retain) CMThreadArg *arg;

@end

@implementation CMThread

@synthesize arg;

static void* pThreadCallback(void* data);

- (id)init
{
    if ((self = [super init]))
    {
        int status = pthread_attr_init(&attr);
        assert(!status);
    }
    
    return self;
}

- (void)dealloc
{
    [self setArg:nil];
    
    pthread_attr_destroy(&attr);
    
    [super dealloc];
}

- (void)start
{
    int status = pthread_create(&posixThreadID, &attr, &pThreadCallback, arg);
    assert(!status);
}

- (void)join
{
    pthread_join(posixThreadID, NULL);
}

+ (void)sleepMilliseconds:(NSInteger)ms;
{
    [NSThread sleepForTimeInterval:ms / 1000.0];
}

static void *pThreadCallback(void *data)
{
    CMThreadArg *arg = (CMThreadArg *)data;
    
    @autoreleasepool
    {
        arg->entryPoint(arg->emulator);
    }
    
    return NULL;
    
}

#pragma mark - blueMSX Callbacks

void* archThreadCreate(void (*entryPoint)(Emulator *), Emulator *emulator, int priority)
{
    CMThreadArg *arg = [[CMThreadArg alloc] init];
    arg->entryPoint = entryPoint;
    arg->emulator = emulator;
    
    CMThread *thread = [[CMThread alloc] init];
    [thread setArg:arg];
    
    [thread start];
    
    return thread;
}

void archThreadJoin(void* thread, int timeout)
{
    [(CMThread *)thread join];
}

void archThreadDestroy(void* thread)
{
    [(CMThread *)thread release];
}

void archThreadSleep(int milliseconds)
{
    [CMThread sleepMilliseconds:milliseconds];
}

@end
