#import <Cocoa/Cocoa.h>
#import "mac_window.h"
#include "plato/console_runner.h"

static void macos_console_clipboard(const char *text, size_t len) {
    (void)len;
    @autoreleasepool {
        NSString *str = [NSString stringWithUTF8String:text];
        if (str) {
            NSPasteboard *pb = [NSPasteboard generalPasteboard];
            [pb clearContents];
            [pb setString:str forType:NSPasteboardTypeString];
        }
    }
}

int main(int argc, const char * argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--v") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("PlatoLives v3.8\n");
            return 0;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--console") == 0 || strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--c") == 0) {
            const char *consoleHost = NULL;
            int consolePort = 0;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                consoleHost = argv[i + 1];
                if (i + 2 < argc && argv[i + 2][0] != '-') {
                    consolePort = atoi(argv[i + 2]);
                }
            }
            plato_console_set_clipboard_callback(macos_console_clipboard);
            return plato_console_run(consoleHost, consolePort);
        }
    }

    @autoreleasepool {
        NSDictionary *spellDefaults = @{
            @"NSAutomaticSpellingCorrectionEnabled": @NO,
            @"NSContinuousSpellCheckingEnabled": @NO,
            @"NSGrammarCheckingEnabled": @NO
        };
        [[NSUserDefaults standardUserDefaults] registerDefaults:spellDefaults];

        NSApplication *app = [NSApplication sharedApplication];
        BOOL isHeadlessTest = NO;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--test-script") == 0) {
                isHeadlessTest = YES;
                break;
            }
        }
        if (isHeadlessTest) {
            [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        } else {
            [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        }
        PLATOAppDelegate *delegate = [[PLATOAppDelegate alloc] init];
        [app setDelegate:delegate];
        if (isHeadlessTest) {
            [app finishLaunching];
            [delegate applicationDidFinishLaunching:[NSNotification notificationWithName:NSApplicationDidFinishLaunchingNotification object:app]];
            while (1) {
                @autoreleasepool {
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
                }
            }
        } else {
            [app run];
        }
    }
    return 0;
}
