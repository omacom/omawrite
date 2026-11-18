#include "fileopenrequests.h"
#include <QGuiApplication>
#include <QPointer>
#import <AppKit/AppKit.h>

// Handle only the native 'rapp' Apple event. ApplicationActivate also occurs
// for Cmd+Tab, window clicks and file opens, none of which request a document.
@interface PWApplicationReopenHandler : NSObject {
@public
    QPointer<FileOpenRequests> requests;
    BOOL installed;
}
- (void)install:(NSNotification *)notification;
- (void)reopen:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply;
@end

@implementation PWApplicationReopenHandler
- (void)install:(NSNotification *)notification {
    Q_UNUSED(notification);
    [[NSAppleEventManager sharedAppleEventManager]
        setEventHandler:self andSelector:@selector(reopen:withReplyEvent:)
        forEventClass:kCoreEventClass andEventID:kAEReopenApplication];
    installed = YES;
}
- (void)reopen:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply {
    Q_UNUSED(event);
    Q_UNUSED(reply);
    if (requests) requests->requestReopen();
}
@end

void installMacReopenHandler(FileOpenRequests *requests) {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa")) return;
    auto *handler = [[PWApplicationReopenHandler alloc] init];
    handler->requests = requests;
    // AppKit may replace handlers during launch, so register again afterwards.
    // Also install now: Qt can already have finished launch without NSApp.run
    // being on the stack. Qt still handles file opens and quitting unchanged.
    [[NSNotificationCenter defaultCenter] addObserver:handler
        selector:@selector(install:) name:NSApplicationDidFinishLaunchingNotification object:NSApp];
    [handler install:nil];
    QObject::connect(requests, &QObject::destroyed, [handler] {
        [[NSNotificationCenter defaultCenter] removeObserver:handler];
        if (handler->installed) [[NSAppleEventManager sharedAppleEventManager]
            removeEventHandlerForEventClass:kCoreEventClass andEventID:kAEReopenApplication];
        [handler release];
    });
}
