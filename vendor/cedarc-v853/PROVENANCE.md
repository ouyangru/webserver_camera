# V853 Cedar encoder runtime

Upstream: https://github.com/lindenis-org/lindenis-v853-external
Pinned commit: bf42476c330aa41aada55e7c030e4be22e46107d
Subdirectory: eyesee-mpp/middleware/sun8iw21/media/LIBRARY/libcedarc/

This is the board vendor's V853 implementation using VencCreate/VencStart and
asynchronous input callbacks. It is distinct from the older Tina multimedia
VideoEncCreate implementation, whose H264 library rejects VE version 0x23010.

Only encoder, memory, base headers/sources and musl encoder/VE libraries were
retrieved. Original upstream copyright notices are preserved. Source and binary
licensing remains governed by the upstream/vendor notices; these files are not
relicensed by this application. They are used locally with the user's SDK.

scripts/build-cedar-runtime.sh builds wrapper libraries in build-v853/lib without
replacing the SDK's multimedia libraries. Runtime dependencies are installed in
/usr/lib/webserver_camera for the firmware package.
