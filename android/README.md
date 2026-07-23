# PS5 Linux Manager — Android companion app (by InsideMatrix)

Native Android app (Java, zero third-party dependencies) for controlling
PS5 Linux Manager and other PS5 web-UI systems (Payload Manager, etaHEN, ...),
plus a raw-TCP `.elf` payload sender.

- Package: `com.insidematrix.ps5linuxmanager`
- minSdk 26 / targetSdk 34 / compileSdk 34
- Dark theme: abyss `#05070B`, ps-blue `#0072CE`, amber `#FFA028`
- Permissions: `INTERNET`, `ACCESS_NETWORK_STATE`; `usesCleartextTraffic="true"`

## Screens
1. **Systems** (`MainActivity`) — saved connections (name/host/port) in
   SharedPreferences (JSON), preset "PS5 Linux Manager — http://\<ip\>:8090",
   add/edit/delete, tap to open console.
2. **WebView console** (`WebConsoleActivity`) — full-screen WebView (JS + DOM
   storage + mixed content enabled), hardware back = web history back, native
   connection-error page with RETRY / EDIT CONNECTION.
3. **Payload Sender** (`PayloadSenderActivity`) — SAF file picker, target
   IP + port (default 9021), streams the file over a TCP socket in 64 KiB
   chunks on a background thread with progress bar, keeps last-used IP/port
   and a 5-entry history.

## Rebuild

Toolchain (verified in this sandbox):

| Component        | Version / source |
|------------------|------------------|
| JDK              | 17 (must include `jlink`, e.g. Amazon Corretto 17 — Debian's `openjdk-17-jre-headless` lacks it and AGP's JdkImageTransform fails) |
| Gradle           | 8.7 (`services.gradle.org/distributions/gradle-8.7-bin.zip`) |
| Android Gradle Plugin | 8.6.1 (from `google()`) |
| cmdline-tools    | `commandlinetools-linux-11076708_latest.zip` |
| SDK packages     | `platform-tools`, `platforms;android-34`, `build-tools;34.0.0` |

Steps:

```sh
# 1) SDK: unpack cmdline-tools to $HOME/android-sdk/cmdline-tools/latest, then:
sdkmanager "platform-tools" "platforms;android-34" "build-tools;34.0.0"
yes | sdkmanager --licenses

# 2) Point the project at the SDK:
echo "sdk.dir=$HOME/android-sdk" > local.properties

# 3) Build (JAVA_HOME must be a FULL JDK 17 with jlink):
export JAVA_HOME=$HOME/jdk17
export ANDROID_HOME=$HOME/android-sdk
~/gradle/gradle-8.7/bin/gradle assembleDebug --no-daemon
```

Output: `app/build/outputs/apk/debug/app-debug.apk` (41 KB).

A convenience script that recreates the whole toolchain from a local zip
cache (survives `$HOME` wipes) lives at `../.toolcache/ensure-android.sh`
(relative to the repo root `ps5-linux-manager/`).

## Signing

Debug-signed with `keystore/debug.keystore` (committed to this tree for
reproducibility): alias `androiddebugkey`, store/key password `android`
(standard Android debug credentials). Wired via `signingConfigs.debug` in
`app/build.gradle`.

## Notes / caveats

- Pure platform SDK (no AndroidX/Material), so the APK is ~41 KB and builds
  offline-of-MavenCentral once AGP is cached.
- The preset entry ships with host `<ip>`; tapping it opens the edit dialog
  so the real PS5 IP can be entered. Cleartext HTTP is enabled for the LAN
  web UIs.
- Pull-to-refresh was omitted (SwipeRefreshLayout would require AndroidX);
  the error page has a RETRY button and the WebView supports reload via the
  page itself.
