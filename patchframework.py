Import("env")
import os
import shutil

# Copies our patched BLEAdvertising.cpp over the vendored copy in the
# framework-arduinopico package cache, every build. Idempotent, and safe
# to run even if PlatformIO re-downloads/updates the framework, since it
# re-applies on top each time rather than relying on the cache persisting.

SRC = os.path.join(env["PROJECT_DIR"], "patches", "BLEAdvertising.cpp")

platform = env.PioPlatform()
framework_dir = platform.get_package_dir("framework-arduinopico")

if framework_dir is None:
    print("[patch_framework] framework-arduinopico not installed, skipping")
elif not os.path.isfile(SRC):
    print(f"[patch_framework] no override found at {SRC}, skipping")
else:
    dst = os.path.join(framework_dir, "libraries", "BLE", "src", "BLEAdvertising.cpp")
    if os.path.isfile(dst):
        print(f"[patch_framework] patching {dst}")
        shutil.copyfile(SRC, dst)
    else:
        print(f"[patch_framework] expected target not found at {dst}, skipping")
