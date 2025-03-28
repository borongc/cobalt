// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_COMMON_UI_CONFIRMATION_ALERT_CONFIRMATION_ALERT_VIEW_CONTROLLER_H_
#define IOS_CHROME_COMMON_UI_CONFIRMATION_ALERT_CONFIRMATION_ALERT_VIEW_CONTROLLER_H_

#import <UIKit/UIKit.h>

@protocol ConfirmationAlertActionHandler;

// A view controller useful to show modal alerts and confirmations. The main
// content consists in a big image, a title, and a subtitle which are contained
// in a scroll view for cases when the content doesn't fit in the screen.
// The view controller can have up to three action buttons, which are positioned
// at the bottom. They are arranged, from top to bottom,
// `primaryActionString`, `secondaryActionString`, `tertiaryActionString`.
// Setting those properties will make those buttons be added to the view
// controller.
@interface ConfirmationAlertViewController : UIViewController

// The navigation bar title view. Nil if not needed. If needed, must be set
// before the view is loaded.
@property(nonatomic, strong) UIView* titleView;

// The view displayed under titles and subtitles. Nil if not needed.
// If needed, must be set before the view is loaded.
@property(nonatomic, strong) UIView* underTitleView;

// The headline below the image. Must be set before the view is loaded.
@property(nonatomic, copy) NSString* titleString;

// Text style for the title. If nil, will default to UIFontTextStyleTitle1.
@property(nonatomic, copy) UIFontTextStyle titleTextStyle;

// (Optional) The additional headline below the main title. Must be set before
// the view is loaded.
@property(nonatomic, copy) NSString* secondaryTitleString;

// The subtitle below the title. Must be set before the view is loaded.
@property(nonatomic, copy) NSString* subtitleString;

// The text for the primary action. Must be set before the view is loaded.
@property(nonatomic, copy) NSString* primaryActionString;

// The text for the secondary action. Must be set before the view is loaded.
@property(nonatomic, copy) NSString* secondaryActionString;

// The color of the text for the secondary action. Must be set before the view
// is loaded to be effective. Use the kBlueColor by default if the property is
// not set.
@property(nonatomic, copy) NSString* secondaryActionTextColor;

// The text for the tertiary action. Must be set before the view is loaded.
@property(nonatomic, copy) NSString* tertiaryActionString;

// The image. May be updated after the view is loaded.
@property(nonatomic, strong) UIImage* image;

// Sets the custom spacing between the top and the image, if there is no
// navigation bar. Must be set before the view is loaded.
@property(nonatomic, assign) CGFloat customSpacingBeforeImageIfNoNavigationBar;

// Sets the custom spacing between the image and the title / subtitle. Must be
// set before the view is loaded.
@property(nonatomic, assign) CGFloat customSpacingAfterImage;

// Sets the custom spacing of the stackview. Values for
// `customSpacingBeforeImageIfNoNavigationBar` and `customSpacingAfterImage` are
// honored around the image, so this applies to all the other items of the
// stackview. Must be set before the view is loaded.
@property(nonatomic, assign) CGFloat customSpacing;

// When YES, the content is attached to the top of the view instead of being
// centered.
@property(nonatomic) BOOL topAlignedLayout;

// Value to determine whether or not the image's size should be scaled.
@property(nonatomic) BOOL imageHasFixedSize;

// Controls if there is a help button in the view. Must be set before the
// view is loaded.
@property(nonatomic) BOOL helpButtonAvailable;

// Set to YES to enclose the image in a frame with a shadow and a corner badge
// with a green checkmark. Must be set before the view is loaded. Default is NO.
@property(nonatomic) BOOL imageEnclosedWithShadowAndBadge;

// Set to NO to prevent the scroll view from showing a vertical scrollbar
// indicator. Must be set before the view is loaded. Default is YES.
@property(nonatomic) BOOL showsVerticalScrollIndicator;

// Set to NO to prevent the scroll view from scrolling. Default is YES.
@property(nonatomic) BOOL scrollEnabled;

// When set, this value will be set as the accessibility label for the help
// button.
@property(nonatomic, copy) NSString* helpButtonAccessibilityLabel;

// The help button item in the top left of the view. Nil if not available.
@property(nonatomic, readonly) UIBarButtonItem* helpButton;

// Controls if the navigation bar dismiss button is available in the view.
// Default is YES. Must be set before the view is loaded.
@property(nonatomic) BOOL showDismissBarButton;

// Allows to modify the system item for the dismiss bar button (defaults to
// UIBarButtonSystemItemDone). Must be set before the view is loaded.
@property(nonatomic, assign) UIBarButtonSystemItem dismissBarButtonSystemItem;

// The action handler for interactions in this View Controller.
@property(nonatomic, weak) id<ConfirmationAlertActionHandler> actionHandler;

// Designated initializer.
- (instancetype)init NS_DESIGNATED_INITIALIZER;

- (instancetype)initWithCoder:(NSCoder*)coder NS_UNAVAILABLE;
- (instancetype)initWithNibName:(NSString*)name
                         bundle:(NSBundle*)bundle NS_UNAVAILABLE;

// Can be overridden by subclasses to customize the secondary title, e.g. set a
// different style, or a UITextViewDelegate. The default implementation does
// nothing.
- (void)customizeSecondaryTitle:(UITextView*)secondaryTitle;

// Can be overridden by subclasses to customize the subtitle, e.g. set a
// different style, or a UITextViewDelegate. The default implementation does
// nothing.
- (void)customizeSubtitle:(UITextView*)subtitle;

// Detent that attempts to fit the preferred height of the content. Detent may
// be inactive in some size classes, so it should be used together with at
// least one other detent.
- (UISheetPresentationControllerDetent*)
    preferredHeightDetent API_AVAILABLE(ios(16));

@end

#endif  // IOS_CHROME_COMMON_UI_CONFIRMATION_ALERT_CONFIRMATION_ALERT_VIEW_CONTROLLER_H_
