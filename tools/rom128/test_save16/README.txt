16MB セーブの単体試験（C）。mGBA の ビルド後に MSYS2 mingw64 で:
  gcc -O1 -DENABLE_VFS -I include -I <build>/include -DM_CORE_GBA tools/rom128/test_save16/savedata16.c src/gba/savedata.c <build>/libmgba.a \
      -lz -lm -lzip -lshlwapi -lole32 -lshell32 -luuid -lws2_32 -lpng -o savedata16.exe
  savedata16.exe <作業フォルダー>      （終了コード 0 ＝ 全部通った・94 項目）
見る事: 6MB の読み書き・読むだけで広げない・バンク 64〜255 の書き読み・RTC の移動・128KB／1MiB／16MB の開き方・ステート。
