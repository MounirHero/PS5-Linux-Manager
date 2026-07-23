# debug.keystore

`app/build.gradle` signs the debug APK with `../keystore/debug.keystore`
(alias `androiddebugkey`, store/key password `android`).

The keystore itself is git-ignored. Regenerate it with:

```sh
keytool -genkeypair -v \
  -keystore debug.keystore \
  -alias androiddebugkey \
  -keyalg RSA -keysize 2048 -validity 10000 \
  -storepass android -keypass android \
  -dname "CN=Android Debug,O=Android,C=US"
```

For a real release, create your own keystore and add a `release` signing
config — never ship production builds signed with a debug key.
