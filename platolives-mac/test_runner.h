#import <Cocoa/Cocoa.h>

@class PLATOView;

@interface PLATOTestRunner : NSObject
- (instancetype)initWithView:(PLATOView *)view scriptPath:(NSString *)path;
- (instancetype)initWithView:(PLATOView *)view scriptString:(NSString *)scriptText;
- (void)start;
@end
