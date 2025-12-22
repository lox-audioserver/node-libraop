{
  "targets": [
    {
      "target_name": "raop_addon",
      "sources": [
        "native/addon.cc",
        "vendor/libraop/src/aes.c",
        "vendor/libraop/src/aes_ctr.c",
        "vendor/libraop/src/alac.c",
        "vendor/libraop/src/bplist.cpp",
        "vendor/libraop/src/pairing.cpp",
        "vendor/libraop/src/password.c",
        "vendor/libraop/src/raop_server.c",
        "vendor/libraop/src/raop_streamer.c",
        "vendor/libraop/src/rtsp_client.c",
        "native/encoder_stub.c",
        "native/log_stub.c",
        "vendor/libraop/crosstools/src/cross_log.c",
        "vendor/libraop/crosstools/src/cross_net.c",
        "vendor/libraop/crosstools/src/cross_ssl.c",
        "vendor/libraop/crosstools/src/cross_thread.c",
        "vendor/libraop/crosstools/src/cross_util.c",
        "vendor/libraop/crosstools/src/platform.c",
        "vendor/libraop/dmap-parser/dmap_parser.c",
        "vendor/libraop/libmdns/mdnssvc/mdns.c",
        "vendor/libraop/libmdns/mdnssvc/mdnsd.c",
        "vendor/libraop/libmdns/mdnssd/mdnssd.c",
        "vendor/libraop/libcodecs/alac/codec/ALACBitUtilities.c",
        "vendor/libraop/libcodecs/alac/codec/ALACDecoder.cpp",
        "vendor/libraop/libcodecs/alac/codec/EndianPortable.c",
        "vendor/libraop/libcodecs/alac/codec/ag_dec.c",
        "vendor/libraop/libcodecs/alac/codec/dp_dec.c",
        "vendor/libraop/libcodecs/alac/codec/matrix_dec.c"
      ],
      "include_dirs": [
        "native",
        "<!(node -p \"require('node-addon-api').include_dir\")",
        "vendor/libraop/src",
        "vendor/libraop/crosstools/src",
        "vendor/libraop/dmap-parser",
        "vendor/libraop/libmdns/mdnssvc",
        "vendor/libraop/libmdns/mdnssd",
        "vendor/libraop/libcodecs/alac/codec"
      ],
      "conditions": [
        [ "OS=='win'", {
          "libraries": [
            "-lssl",
            "-lcrypto",
            "-lws2_32",
            "-liphlpapi",
            "-lbcrypt"
          ]
        }],
        [ "OS!='win'", {
          "libraries": [
            "-L/opt/homebrew/opt/openssl@3/lib",
            "-L/usr/local/opt/openssl@3/lib",
            "<!(python3 -c \"import os; candidates=['/usr/lib/aarch64-linux-gnu/libssl.so.3','/usr/lib/x86_64-linux-gnu/libssl.so.3','/usr/lib64/libssl.so','/usr/lib/libssl.so']; print(next((c for c in candidates if os.path.exists(c)),'-lssl'))\")",
            "<!(python3 -c \"import os; candidates=['/usr/lib/aarch64-linux-gnu/libcrypto.so.3','/usr/lib/x86_64-linux-gnu/libcrypto.so.3','/usr/lib64/libcrypto.so','/usr/lib/libcrypto.so']; print(next((c for c in candidates if os.path.exists(c)),'-lcrypto'))\")",
            "-lpthread",
            "-ldl",
            "-lm"
          ]
        }],
        [ "OS=='mac'", {
          "xcode_settings": { "OTHER_LDFLAGS": [ "-framework", "CoreFoundation" ] }
        }]
      ],
      "actions": [
        {
          "action_name": "prepare_libraop",
          "inputs": [],
          "outputs": [ "<(module_root_dir)/vendor/libraop/.prepared" ],
          "action": [ "bash", "<(module_root_dir)/scripts/prepare-libraop.sh" ]
        }
      ],
      "cflags": [ "-fPIC" ],
      "cflags_cc": [ "-fPIC", "-std=c++17" ],
      "cflags!": [ "-fno-exceptions" ],
      "cflags_cc!": [ "-fno-exceptions" ],
      "defines": [ "NAPI_DISABLE_CPP_EXCEPTIONS", "_GNU_SOURCE", "OPENSSL_SUPPRESS_DEPRECATED", "PCM_ONLY", "NAPI_VERSION=8" ],
      "dependencies": []
    }
  ]
}
