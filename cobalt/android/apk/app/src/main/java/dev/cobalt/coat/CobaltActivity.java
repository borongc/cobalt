// Copyright 2017 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package dev.cobalt.coat;

import static dev.cobalt.util.Log.TAG;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageManager.NameNotFoundException;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.TextUtils;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup.LayoutParams;
import android.view.ViewParent;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.Toast;
import androidx.annotation.Nullable;
import dev.cobalt.coat.javabridge.CobaltJavaScriptAndroidObject;
import dev.cobalt.coat.javabridge.CobaltJavaScriptInterface;
import dev.cobalt.coat.javabridge.H5vccPlatformService;
import dev.cobalt.coat.javabridge.HTMLMediaElementExtension;
import dev.cobalt.media.AudioOutputManager;
import dev.cobalt.media.MediaCodecCapabilitiesLogger;
import dev.cobalt.media.VideoSurfaceView;
import dev.cobalt.shell.Shell;
import dev.cobalt.shell.ShellManager;
import dev.cobalt.util.DisplayUtil;
import dev.cobalt.util.Log;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.regex.Pattern;
import org.chromium.base.CommandLine;
import org.chromium.base.library_loader.LibraryLoader;
import org.chromium.base.library_loader.LibraryProcessType;
import org.chromium.components.version_info.VersionInfo;
import org.chromium.content.browser.input.ImeAdapterImpl;
import org.chromium.content_public.browser.BrowserStartupController;
import org.chromium.content_public.browser.DeviceUtils;
import org.chromium.content_public.browser.JavascriptInjector;
import org.chromium.content_public.browser.WebContents;
import org.chromium.ui.base.ActivityWindowAndroid;
import org.chromium.ui.base.IntentRequestTracker;

/** Native activity that has the required JNI methods called by the Starboard implementation. */
public abstract class CobaltActivity extends Activity {
  private static final String URL_ARG = "--url=";
  private static final String META_DATA_APP_URL = "cobalt.APP_URL";

  // This key differs in naming format for legacy reasons
  public static final String COMMAND_LINE_ARGS_KEY = "commandLineArgs";

  private static final Pattern URL_PARAM_PATTERN = Pattern.compile("^[a-zA-Z0-9_=]*$");

  // Maintain the list of JavaScript-exposed objects as a member variable
  // to prevent them from being garbage collected prematurely.
  private List<CobaltJavaScriptAndroidObject> javaScriptAndroidObjectList = new ArrayList<>();

  @SuppressWarnings("unused")
  private CobaltA11yHelper a11yHelper;

  private VideoSurfaceView videoSurfaceView;

  private boolean forceCreateNewVideoSurfaceView;

  private long timeInNanoseconds;

  private ShellManager mShellManager;
  private ActivityWindowAndroid mWindowAndroid;
  private Intent mLastSentIntent;
  private String mStartupUrl;
  private IntentRequestTracker mIntentRequestTracker;
  protected Boolean shouldSetJNIPrefix = true;
  // Tracks the status of the FLAG_KEEP_SCREEN_ON window flag.
  private Boolean isKeepScreenOnEnabled = false;


  // Initially copied from ContentShellActiviy.java
  protected void createContent(final Bundle savedInstanceState) {
    // Initializing the command line must occur before loading the library.
    if (!CommandLine.isInitialized()) {
      CommandLine.init(null);

      String[] commandLineArgs =
          getCommandLineParamsFromIntent(
              getIntent(), COMMAND_LINE_ARGS_KEY);
      CommandLineOverrideHelper.getFlagOverrides(
          new CommandLineOverrideHelper.CommandLineOverrideHelperParams(
              shouldSetJNIPrefix,
              VersionInfo.isOfficialBuild(),
              commandLineArgs
        ));
    }

    DeviceUtils.addDeviceSpecificUserAgentSwitch();

    // This initializes JNI and ends up calling JNI_OnLoad in native code
    LibraryLoader.getInstance().ensureInitialized();

    // StarboardBridge initialization must happen right after library loading,
    // before Browser/Content module is started. It currently tracks its own JNI state
    // variables, and needs to set up an early copy.

    // TODO(b/374147993): how to handle deeplink in Chrobalt?
    String startDeepLink = getIntentUrlAsString(getIntent());
    if (startDeepLink == null) {
      throw new IllegalArgumentException("startDeepLink cannot be null, set it to empty string");
    }
    if (getStarboardBridge() == null) {
      // Cold start - Instantiate the singleton StarboardBridge.
      StarboardBridge starboardBridge = createStarboardBridge(getArgs(), startDeepLink);
      ((StarboardBridge.HostApplication) getApplication()).setStarboardBridge(starboardBridge);
    } else {
      // Warm start - Pass the deep link to the running Starboard app.
      getStarboardBridge().handleDeepLink(startDeepLink);
    }

    mShellManager = new ShellManager(this);
    final boolean listenToActivityState = true;
    mIntentRequestTracker = IntentRequestTracker.createFromActivity(this);
    mWindowAndroid = new ActivityWindowAndroid(this, listenToActivityState, mIntentRequestTracker);
    mIntentRequestTracker.restoreInstanceState(savedInstanceState);
    mShellManager.setWindow(mWindowAndroid);
    setContentView(mShellManager.getContentViewRenderView());
    // Set up the animation placeholder to be the SurfaceView. This disables the
    // SurfaceView's 'hole' clipping during animations that are notified to the window.
    mWindowAndroid.setAnimationPlaceholderView(
        mShellManager.getContentViewRenderView().getSurfaceView());

    if (mStartupUrl == null || mStartupUrl.isEmpty()) {
      String[] args = getStarboardBridge().getArgs();
      mStartupUrl =
          Arrays.stream(args)
              .filter(line -> line.contains(URL_ARG))
              .findAny()
              .map(arg -> arg.substring(arg.indexOf(URL_ARG) + URL_ARG.length()))
              .orElse(null);
    }
    if (!TextUtils.isEmpty(mStartupUrl)) {
      mShellManager.setStartupUrl(Shell.sanitizeUrl(mStartupUrl));
    }

    // TODO(b/377025559): Bring back WebTests launch capability
    BrowserStartupController.getInstance()
        .startBrowserProcessesAsync(
            LibraryProcessType.PROCESS_BROWSER,
            false, // Do not start a separate GPU process
            // TODO(b/377025565): Figure out what this means
            false, // Do not start in "minimal" or paused mode
            new BrowserStartupController.StartupCallback() {
              @Override
              public void onSuccess() {
                Log.i(TAG, "Browser process init succeeded");
                finishInitialization(savedInstanceState);
                getStarboardBridge().measureAppStartTimestamp();
              }

              @Override
              public void onFailure() {
                Log.e(TAG, "Browser process init failed");
                initializationFailed();
              }
            });
  }

  // Initially copied from ContentShellActiviy.java
  private void finishInitialization(Bundle savedInstanceState) {
    // Load an empty page to let shell create WebContents.
    mShellManager.launchShell("");
    // Inject JavaBridge objects to the WebContents.
    initializeJavaBridge();
    getStarboardBridge().setWebContents(getActiveWebContents());

    // Load the `url` with the same shell we created above.
    Log.i(TAG, "shellManager load url:" + mStartupUrl);
    mShellManager.getActiveShell().loadUrl(mStartupUrl);
  }

  // Initially copied from ContentShellActiviy.java
  private void initializationFailed() {
    Log.e(TAG, "ContentView initialization failed.");
    Toast.makeText(
            CobaltActivity.this, R.string.browser_process_initialization_failed, Toast.LENGTH_SHORT)
        .show();
    finish();
  }

  protected boolean dispatchKeyEventToIme(int keyCode, int action) {
    ImeAdapterImpl imeAdapter = getImeAdapterImpl();
    if (imeAdapter == null) {
      return false;
    }

    // |KeyEvent| needs to be created with |downTime| and |eventTime| set. If they are not set the
    // app closes.
    long eventTime = SystemClock.uptimeMillis();
    return imeAdapter.dispatchKeyEvent(new KeyEvent(eventTime, eventTime, action, keyCode, 0));
  }

  @Override
  public boolean onKeyDown(int keyCode, KeyEvent event) {
    return dispatchKeyEventToIme(keyCode, KeyEvent.ACTION_DOWN) || super.onKeyDown(keyCode, event);
  }

  @Override
  public boolean onKeyUp(int keyCode, KeyEvent event) {
    return dispatchKeyEventToIme(keyCode, KeyEvent.ACTION_UP) || super.onKeyUp(keyCode, event);
  }

  // Initially copied from ContentShellActiviy.java
  @Override
  public void onActivityResult(int requestCode, int resultCode, Intent data) {
    super.onActivityResult(requestCode, resultCode, data);
    mIntentRequestTracker.onActivityResult(requestCode, resultCode, data);
  }

  // Initially copied from ContentShellActiviy.java
  @Override
  public void startActivity(Intent i) {
    mLastSentIntent = i;
    super.startActivity(i);
  }

  public Intent getLastSentIntent() {
    return mLastSentIntent;
  }

  private static String getUrlFromIntent(Intent intent) {
    return intent != null ? intent.getDataString() : null;
  }

  private static String[] getCommandLineParamsFromIntent(Intent intent, String key) {
    return intent != null ? intent.getStringArrayExtra(key) : null;
  }

  /**
   * @return The {@link ShellManager} configured for the activity or null if it has not been created
   *     yet.
   */
  public ShellManager getShellManager() {
    return mShellManager;
  }

  /**
   * @return The currently visible {@link Shell} or null if one is not showing.
   */
  public Shell getActiveShell() {
    return mShellManager != null ? mShellManager.getActiveShell() : null;
  }

  /**
   * @return The {@link WebContents} owned by the currently visible {@link Shell} or null if one is
   *     not showing.
   */
  @Nullable
  public WebContents getActiveWebContents() {
    Shell shell = getActiveShell();
    return shell != null ? shell.getWebContents() : null;
  }

  @Nullable
  protected ImeAdapterImpl getImeAdapterImpl() {
    WebContents webContents = getActiveWebContents();
    return webContents != null ? ImeAdapterImpl.fromWebContents(webContents) : null;
  }

  // TODO(b/375442742): re-enable native code.
  // private static native void nativeLowMemoryEvent();

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    // Record the application start timestamp.
    timeInNanoseconds = System.nanoTime();

    // To ensure that volume controls adjust the correct stream, make this call
    // early in the app's lifecycle. This connects the volume controls to
    // STREAM_MUSIC whenever the target activity or fragment is visible.
    setVolumeControlStream(AudioManager.STREAM_MUSIC);

    super.onCreate(savedInstanceState);
    createContent(savedInstanceState);

    videoSurfaceView = new VideoSurfaceView(this);
    a11yHelper = new CobaltA11yHelper(this, videoSurfaceView);
    addContentView(videoSurfaceView, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
  }

  /**
   * Initializes the Java Bridge to allow communication between Java and JavaScript. This method
   * injects Java objects into the WebView and loads corresponding JavaScript code.
   */
  private void initializeJavaBridge() {
    Log.i(TAG, "initializeJavaBridge");

    WebContents webContents = getActiveWebContents();
    if (webContents == null) {
      throw new RuntimeException(
          "webContents is null in initializeJavaBridge. This should never happen.");
    }

    // --- Initialize the Java Bridge ---

    // 1. Gather all Java objects that need to be exposed to JavaScript.
    // TODO(b/379701165): consider to refine the way to add JavaScript interfaces.
    javaScriptAndroidObjectList.add(new H5vccPlatformService(this, getStarboardBridge()));
    javaScriptAndroidObjectList.add(new HTMLMediaElementExtension(this));

    // 2. Use JavascriptInjector to inject Java objects into the WebContents.
    //    This makes the annotated methods in these objects accessible from JavaScript.
    JavascriptInjector javascriptInjector = JavascriptInjector.fromWebContents(webContents, false);

    javascriptInjector.setAllowInspection(true);
    for (CobaltJavaScriptAndroidObject javascriptAndroidObject : javaScriptAndroidObjectList) {
      Log.d(
          TAG,
          "Add JavaScriptAndroidObject:" + javascriptAndroidObject.getJavaScriptInterfaceName());
      javascriptInjector.addPossiblyUnsafeInterface(
          javascriptAndroidObject,
          javascriptAndroidObject.getJavaScriptInterfaceName(),
          CobaltJavaScriptInterface.class);
    }
  }

  /**
   * Instantiates the StarboardBridge. Apps not supporting sign-in should inject an instance of
   * NoopUserAuthorizer. Apps may subclass StarboardBridge if they need to override anything.
   */
  protected abstract StarboardBridge createStarboardBridge(String[] args, String startDeepLink);

  protected StarboardBridge getStarboardBridge() {
    return ((StarboardBridge.HostApplication) getApplication()).getStarboardBridge();
  }

  @Override
  protected void onStart() {
    if (!isReleaseBuild()) {
      getStarboardBridge().getAudioOutputManager().dumpAllOutputDevices();
      MediaCodecCapabilitiesLogger.dumpAllDecoders();
    }
    if (forceCreateNewVideoSurfaceView) {
      Log.w(TAG, "Force to create a new video surface.");
      createNewSurfaceView();
    }

    DisplayUtil.cacheDefaultDisplay(this);
    DisplayUtil.addDisplayListener(this);
    AudioOutputManager.addAudioDeviceListener(this);

    getStarboardBridge().onActivityStart(this);
    super.onStart();

    WebContents webContents = getActiveWebContents();
    if (webContents != null) {
      // document.onresume event
      webContents.onResume();
      // visibility:visible event
      webContents.onShow();
    }
  }

  @Override
  protected void onStop() {
    getStarboardBridge().onActivityStop(this);
    super.onStop();

    WebContents webContents = getActiveWebContents();
    if (webContents != null) {
      // visibility:hidden event
      webContents.onHide();
      // document.onfreeze event
      webContents.onFreeze();
    }

    if (VideoSurfaceView.getCurrentSurface() != null) {
      forceCreateNewVideoSurfaceView = true;
    }

    // Set the SurfaceView to fullscreen.
    View rootView = getWindow().getDecorView();
    setVideoSurfaceBounds(0, 0, rootView.getWidth(), rootView.getHeight());
  }

  @Override
  protected void onDestroy() {
    if (mShellManager != null) {
      mShellManager.destroy();
    }
    mWindowAndroid.destroy();
    super.onDestroy();
    getStarboardBridge().onActivityDestroy(this);
  }

  @Override
  public boolean onSearchRequested() {
    return getStarboardBridge().onSearchRequested();
  }

  /** Returns true if the argument list contains an arg starting with argName. */
  private static boolean hasArg(List<String> args, String argName) {
    for (String arg : args) {
      if (arg.startsWith(argName)) {
        return true;
      }
    }
    return false;
  }

  /**
   * Get argv/argc style args, if any from intent extras. Returns empty array if there are none
   *
   * <p>To use, invoke application via, eg, adb shell am start --esa args arg1,arg2 \
   * dev.cobalt.coat/dev.cobalt.app.MainActivity
   */
  protected String[] getArgs() {
    ArrayList<String> args = new ArrayList<>();
    String[] commandLineArgs = getCommandLineParamsFromIntent(getIntent(), COMMAND_LINE_ARGS_KEY);
    if (commandLineArgs != null) {
      args.addAll(Arrays.asList(commandLineArgs));
    }

    // If the URL arg isn't specified, get it from AndroidManifest.xml.
    boolean hasUrlArg = hasArg(args, URL_ARG);
    if (!hasUrlArg) {
      try {
        ActivityInfo ai =
            getPackageManager()
                .getActivityInfo(getIntent().getComponent(), PackageManager.GET_META_DATA);
        if (ai.metaData != null) {
          if (!hasUrlArg) {
            String url = ai.metaData.getString(META_DATA_APP_URL);
            if (url != null) {
              args.add(URL_ARG + url);
            }
          }
        }
      } catch (NameNotFoundException e) {
        throw new RuntimeException("Error getting activity info", e);
      }
    }

    Bundle extras = getIntent().getExtras();
    CharSequence[] urlParams = (extras == null) ? null : extras.getCharSequenceArray("url_params");
    if (urlParams != null) {
      appendUrlParamsToUrl(args, urlParams);
    }

    return args.toArray(new String[0]);
  }

  private void appendUrlParamsToUrl(List<String> args, CharSequence[] urlParams) {
    int idx = -1;
    for (int i = 0; i < args.size(); i++) {
      if (args.get(i).startsWith(URL_ARG)) {
        idx = i;
        break;
      }
    }

    if (idx >= 0) {
      StringBuilder urlBuilder = new StringBuilder();
      urlBuilder.append(args.get(idx));
      // append & if ? is already in the url, otherwise append ?
      if (urlBuilder.indexOf("?") > 0) {
        urlBuilder.append("&");
      } else {
        urlBuilder.append("?");
      }

      for (int j = 0; j < urlParams.length; j++) {
        // sanitize the input before append to the url.
        String paramKeyValuePair = urlParams[j].toString();
        if (URL_PARAM_PATTERN.matcher(paramKeyValuePair).matches()) {
          urlBuilder.append(paramKeyValuePair);
          urlBuilder.append('&');
        }
      }

      urlBuilder.deleteCharAt(urlBuilder.length() - 1);
      args.set(idx, urlBuilder.toString());
    }
  }

  protected boolean isReleaseBuild() {
    return StarboardBridge.isReleaseBuild();
  }

  @Override
  protected void onNewIntent(Intent intent) {
    getStarboardBridge().handleDeepLink(getIntentUrlAsString(intent));
  }

  /**
   * Returns the URL from an Intent as a string. This may be overridden for additional processing.
   */
  protected String getIntentUrlAsString(Intent intent) {
    Uri intentUri = intent.getData();
    return (intentUri == null) ? "" : intentUri.toString();
  }

  @Override
  public void onRequestPermissionsResult(
      int requestCode, String[] permissions, int[] grantResults) {
    getStarboardBridge().onRequestPermissionsResult(requestCode, permissions, grantResults);
  }

  public void resetVideoSurface() {
    runOnUiThread(
        new Runnable() {
          @Override
          public void run() {
            createNewSurfaceView();
          }
        });
  }

  public void setVideoSurfaceBounds(final int x, final int y, final int width, final int height) {
    if (width == 0 || height == 0) {
      // The SurfaceView should be covered by our UI layer in this case.
      return;
    }
    runOnUiThread(
        new Runnable() {
          @Override
          public void run() {
            LayoutParams layoutParams = videoSurfaceView.getLayoutParams();
            // Since videoSurfaceView is added directly to the Activity's content view, which is a
            // FrameLayout, we expect its layout params to become FrameLayout.LayoutParams.
            if (layoutParams instanceof FrameLayout.LayoutParams) {
              ((FrameLayout.LayoutParams) layoutParams).setMargins(x, y, x + width, y + height);
            } else {
              Log.w(
                  TAG,
                  "Unexpected video surface layout params class "
                      + layoutParams.getClass().getName());
            }
            layoutParams.width = width;
            layoutParams.height = height;
            // Even though as a NativeActivity we're not using the Android UI framework, by setting
            // the  layout params it will force a layout to be requested. That will cause the
            // SurfaceView to position its underlying Surface to match the screen coordinates of
            // where the view would be in a UI layout and to set the surface transform matrix to
            // match the view's size.
            videoSurfaceView.setLayoutParams(layoutParams);
          }
        });
  }

  private void createNewSurfaceView() {
    ViewParent parent = videoSurfaceView.getParent();
    if (parent instanceof FrameLayout) {
      FrameLayout frameLayout = (FrameLayout) parent;
      int index = frameLayout.indexOfChild(videoSurfaceView);
      frameLayout.removeView(videoSurfaceView);
      Log.i(TAG, "removed videoSurfaceView at index:" + index);

      videoSurfaceView = new VideoSurfaceView(this);
      a11yHelper = new CobaltA11yHelper(this, videoSurfaceView);
      frameLayout.addView(
          videoSurfaceView,
          index,
          new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));
      Log.i(TAG, "inserted new videoSurfaceView at index:" + index);
    } else {
      Log.w(TAG, "Unexpected surface view parent class " + parent.getClass().getName());
    }
  }

  @Override
  public void onLowMemory() {
    super.onLowMemory();
    // TODO(cobalt): re-enable native low memory event or remove code if unnecessary.
    // nativeLowMemoryEvent();
  }

  public long getAppStartTimestamp() {
    return timeInNanoseconds;
  }

  public void evaluateJavaScript(String jsCode) {
    // evaluateJavaScript must run on UI thread.
    runOnUiThread(
        new Runnable() {
          @Override
          public void run() {
            WebContents webContents = getActiveWebContents();
            if (webContents != null) {
              webContents.evaluateJavaScript(jsCode, null);
            }
          }
        });
  }

  public void toggleKeepScreenOn(boolean keepOn) {
    if (isKeepScreenOnEnabled != keepOn) {
      runOnUiThread(new Runnable() {
            @Override
            public void run() {
              if (keepOn) {
                getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                Log.i(TAG, "Screen keep-on enabled for video playback");
              } else {
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                Log.i(TAG, "Screen keep-on disabled");
              }
            }
          });
      isKeepScreenOnEnabled = keepOn;
    }
  }
}
