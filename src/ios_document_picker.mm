#include "ios_document_picker.h"

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

#if TARGET_OS_IOS

@interface FFmpegGDIOSDocumentPickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, assign) IOSDocumentPicker *owner;
@property(nonatomic, retain) NSURL *currentURL;
@property(nonatomic, assign) BOOL currentURLAccessing;
- (instancetype)initWithOwner:(IOSDocumentPicker *)owner;
- (void)presentMediaPicker;
- (void)stopAccessingCurrentURL;
@end

@implementation FFmpegGDIOSDocumentPickerDelegate

- (UIViewController *)currentRootViewController {
    if (@available(iOS 13.0, *)) {
        for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
            if (scene.activationState != UISceneActivationStateForegroundActive || ![scene isKindOfClass:[UIWindowScene class]]) {
                continue;
            }
            UIWindowScene *window_scene = (UIWindowScene *)scene;
            for (UIWindow *window in window_scene.windows) {
                if (!window.keyWindow) {
                    continue;
                }
                UIViewController *controller = window.rootViewController;
                while (controller.presentedViewController) {
                    controller = controller.presentedViewController;
                }
                return controller;
            }
        }
    } else {
        for (UIWindow *window in [UIApplication sharedApplication].windows) {
            if (!window.keyWindow) {
                continue;
            }
            UIViewController *controller = window.rootViewController;
            while (controller.presentedViewController) {
                controller = controller.presentedViewController;
            }
            return controller;
        }
    }
    return nil;
}

- (instancetype)initWithOwner:(IOSDocumentPicker *)owner {
    self = [super init];
    if (self) {
        _owner = owner;
    }
    return self;
}

- (void)presentMediaPicker {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *root_controller = [self currentRootViewController];
        if (!root_controller) {
            self.owner->emit_canceled();
            return;
        }

        UIDocumentPickerViewController *picker = nil;
        if (@available(iOS 14.0, *)) {
            NSArray<UTType *> *types = @[
                UTTypeItem
            ];
            picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types asCopy:NO];
        } else {
            NSArray<NSString *> *types = @[
                @"public.item"
            ];
            picker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:types inMode:UIDocumentPickerModeOpen];
        }
        picker.delegate = self;
        picker.allowsMultipleSelection = NO;
        [root_controller presentViewController:picker animated:YES completion:nil];
        [picker release];
    });
}

- (void)stopAccessingCurrentURL {
    if (self.currentURLAccessing && self.currentURL) {
        [self.currentURL stopAccessingSecurityScopedResource];
    }
    self.currentURLAccessing = NO;
    self.currentURL = nil;
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *url = urls.firstObject;
    if (!url) {
        self.owner->emit_canceled();
        return;
    }
    [self stopAccessingCurrentURL];
    BOOL accessing = [url startAccessingSecurityScopedResource];
    if (!url.path) {
        if (accessing) {
            [url stopAccessingSecurityScopedResource];
        }
        self.owner->emit_canceled();
        return;
    }
    self.currentURL = url;
    self.currentURLAccessing = accessing;
    self.owner->emit_file_selected(String::utf8(url.path.UTF8String));
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    self.owner->emit_canceled();
}

@end

#endif

void IOSDocumentPicker::_bind_methods() {
    ClassDB::bind_method(D_METHOD("open_media"), &IOSDocumentPicker::open_media);

    ADD_SIGNAL(MethodInfo("file_selected", PropertyInfo(Variant::STRING, "path")));
    ADD_SIGNAL(MethodInfo("canceled"));
}

void IOSDocumentPicker::open_media() {
#if TARGET_OS_IOS
    FFmpegGDIOSDocumentPickerDelegate *picker_delegate = (FFmpegGDIOSDocumentPickerDelegate *)delegate;
    [picker_delegate presentMediaPicker];
#else
    emit_canceled();
#endif
}

void IOSDocumentPicker::emit_file_selected(const String &p_path) {
    emit_signal("file_selected", p_path);
}

void IOSDocumentPicker::emit_canceled() {
    emit_signal("canceled");
}

IOSDocumentPicker::IOSDocumentPicker() {
#if TARGET_OS_IOS
    delegate = [[FFmpegGDIOSDocumentPickerDelegate alloc] initWithOwner:this];
#endif
}

IOSDocumentPicker::~IOSDocumentPicker() {
#if TARGET_OS_IOS
    FFmpegGDIOSDocumentPickerDelegate *picker_delegate = (FFmpegGDIOSDocumentPickerDelegate *)delegate;
    [picker_delegate stopAccessingCurrentURL];
    [picker_delegate release];
    delegate = nullptr;
#endif
}
